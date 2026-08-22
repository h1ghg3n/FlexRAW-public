#include "resource_router_admission.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include <QDateTime>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace flexraw::worker::admission
{
namespace
{

struct HttpResponse
{
    int statusCode{0};
    QByteArray body;
    QString diagnostic;
    bool timedOut{false};
};

enum class HttpMethod : std::uint8_t
{
    Post,
    Delete,
};

constexpr qsizetype MaximumRouterDiagnosticLength = 512;

// 목적: Router response 또는 transport diagnostic을 bounded UTF-8 diagnostic으로 변환
// 입력: text: Qt network 또는 JSON response text
// 출력: wire 또는 log에 안전한 최대 길이 std::string
[[nodiscard]] std::string toBoundedDiagnostic(QString text)
{
    if (text.size() > MaximumRouterDiagnosticLength)
    {
        text.truncate(MaximumRouterDiagnosticLength);
    }
    return text.toStdString();
}

// 목적: configured base URL 아래의 Resource Router v1 path 생성
// 입력: endpoint: operator가 설정한 Router base URL, pathSuffix: /v1 이하 path
// 출력: HTTP(S) host를 가진 endpoint면 request URL, 아니면 invalid QUrl
[[nodiscard]] QUrl makeRouterUrl(const std::string& endpoint, const QString& pathSuffix)
{
    QUrl url(QString::fromStdString(endpoint));
    if (!url.isValid() || (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https")) ||
        url.host().isEmpty())
    {
        return {};
    }

    QString basePath = url.path();
    while (basePath.endsWith(u'/'))
    {
        basePath.chop(1);
    }
    url.setPath(basePath + pathSuffix);
    return url;
}

// 목적: local QEventLoop에서 하나의 bounded HTTP request를 완료까지 실행
// 입력: method/url/body/timeout: Router operation과 bounded wait configuration
// 출력: HTTP status/body 또는 timeout/transport diagnostic
[[nodiscard]] HttpResponse executeRequest(const HttpMethod method,
                                          const QUrl& url,
                                          const QByteArray& body,
                                          const std::chrono::milliseconds timeout)
{
    if (!url.isValid() || timeout <= std::chrono::milliseconds::zero() ||
        timeout > std::chrono::milliseconds{std::numeric_limits<int>::max()})
    {
        return {0, {}, QStringLiteral("Resource Router endpoint or request timeout is invalid."), false};
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply* reply = method == HttpMethod::Post ? manager.post(request, body) : manager.deleteResource(request);

    QEventLoop eventLoop;
    QTimer timer;
    timer.setSingleShot(true);
    bool timedOut = false;
    QObject::connect(reply, &QNetworkReply::finished, &eventLoop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, reply, [&]() {
        timedOut = true;
        reply->abort();
    });
    timer.start(static_cast<int>(timeout.count()));
    if (!reply->isFinished())
    {
        eventLoop.exec();
    }
    timer.stop();

    const QVariant statusAttribute = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const int statusCode = statusAttribute.isValid() ? statusAttribute.toInt() : 0;
    const QByteArray responseBody = reply->readAll();
    const QString diagnostic = timedOut ? QStringLiteral("Resource Router request timed out.") : reply->errorString();
    return {statusCode, responseBody, diagnostic, timedOut};
}

// 목적: Router JSON object response를 parsing하고 malformed response를 구분
// 입력: body: HTTP response body
// 출력: top-level JSON object 또는 nullopt
[[nodiscard]] std::optional<QJsonObject> parseJsonObject(const QByteArray& body)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
    {
        return std::nullopt;
    }
    return document.object();
}

// 목적: Router RFC3339 expiry text를 C++ system clock timestamp로 변환
// 입력: text: Router가 authoritative하게 반환한 expires_at
// 출력: 유효한 UTC instant 또는 nullopt
[[nodiscard]] std::optional<std::chrono::system_clock::time_point> parseExpiry(const QString& text)
{
    const QDateTime dateTime = QDateTime::fromString(text, Qt::ISODate);
    if (!dateTime.isValid())
    {
        return std::nullopt;
    }
    return std::chrono::system_clock::time_point{std::chrono::milliseconds{dateTime.toMSecsSinceEpoch()}};
}

// 목적: Router acquire/renew success JSON에서 active lease 추출
// 입력: object: { lease: {...} } 또는 top-level lease representation
// 출력: lease_id와 expires_at이 모두 유효하면 portable lease value
[[nodiscard]] std::optional<RenderResourceLease> parseLease(const QJsonObject& object)
{
    const QJsonObject leaseObject =
        object.contains(QStringLiteral("lease")) ? object.value(QStringLiteral("lease")).toObject() : object;
    const QString leaseId = leaseObject.value(QStringLiteral("lease_id")).toString();
    const std::optional<std::chrono::system_clock::time_point> expiry =
        parseExpiry(leaseObject.value(QStringLiteral("expires_at")).toString());
    if (leaseId.isEmpty() || !expiry.has_value())
    {
        return std::nullopt;
    }
    return RenderResourceLease{leaseId.toStdString(), *expiry};
}

// 목적: Router error object에서 protocol-stable diagnostic code를 우선 추출
// 입력: object: BUSY 또는 common error response JSON
// 출력: reason/code/message 중 하나의 bounded diagnostic text
[[nodiscard]] std::string extractRouterDiagnostic(const QJsonObject& object)
{
    QString diagnostic = object.value(QStringLiteral("reason")).toString();
    if (diagnostic.isEmpty())
    {
        diagnostic = object.value(QStringLiteral("code")).toString();
    }
    if (diagnostic.isEmpty())
    {
        diagnostic = object.value(QStringLiteral("message")).toString();
    }
    return toBoundedDiagnostic(diagnostic.isEmpty() ? QStringLiteral("Resource Router returned an error.")
                                                    : diagnostic);
}

// 목적: render claim이 Router v1 ResourceVector 최소 요구를 만족하는지 검증
// 입력: request: client id, TTL과 memory/GPU/CPU claim
// 출력: 위반 시 InvalidRequest decision, 유효하면 nullopt
[[nodiscard]] std::optional<ResourceAdmissionDecision> validateAcquireRequest(const ResourceLeaseRequest& request)
{
    if (request.requestId.empty() || request.clientId.empty())
    {
        return ResourceAdmissionDecision{ResourceAdmissionStatus::InvalidRequest,
                                         std::nullopt,
                                         std::nullopt,
                                         "Resource Router lease request requires request and client IDs."};
    }
    if (request.ttl <= std::chrono::seconds::zero())
    {
        return ResourceAdmissionDecision{ResourceAdmissionStatus::InvalidRequest,
                                         std::nullopt,
                                         std::nullopt,
                                         "Resource Router lease TTL must be positive."};
    }
    if (!std::isfinite(request.claim.cpuCores) || request.claim.cpuCores < 0.0)
    {
        return ResourceAdmissionDecision{ResourceAdmissionStatus::InvalidRequest,
                                         std::nullopt,
                                         std::nullopt,
                                         "Resource Router CPU claim is invalid."};
    }
    if (request.claim.memoryMiB == 0 && !request.claim.requiresGpu && request.claim.cpuCores == 0.0)
    {
        return ResourceAdmissionDecision{ResourceAdmissionStatus::InvalidRequest,
                                         std::nullopt,
                                         std::nullopt,
                                         "Resource Router claim must request memory, GPU, or CPU capacity."};
    }
    if (request.claim.memoryMiB > static_cast<std::uint64_t>(std::numeric_limits<qint64>::max()))
    {
        return ResourceAdmissionDecision{ResourceAdmissionStatus::InvalidRequest,
                                         std::nullopt,
                                         std::nullopt,
                                         "Resource Router memory claim exceeds JSON integer support."};
    }
    return std::nullopt;
}

// 목적: Router BUSY response의 optional retry_after_ms를 portable duration으로 변환
// 입력: object: BUSY response JSON
// 출력: non-negative integer가 있으면 advisory retry duration
[[nodiscard]] std::optional<std::chrono::milliseconds> parseRetryAfter(const QJsonObject& object)
{
    const QJsonValue value = object.value(QStringLiteral("retry_after_ms"));
    if (!value.isDouble())
    {
        return std::nullopt;
    }
    const qint64 milliseconds = value.toInteger(-1);
    if (milliseconds < 0)
    {
        return std::nullopt;
    }
    return std::chrono::milliseconds{milliseconds};
}

// 목적: Router HTTP/transport result를 shared admission decision으로 map
// 입력: response: bounded HTTP operation result
// 출력: BUSY는 Busy, 4xx validation은 InvalidRequest, 나머지는 Unavailable
[[nodiscard]] ResourceAdmissionDecision makeFailureDecision(const HttpResponse& response)
{
    const std::optional<QJsonObject> body = parseJsonObject(response.body);
    if (response.statusCode == 409 && body.has_value() &&
        body->value(QStringLiteral("status")).toString() == QStringLiteral("BUSY"))
    {
        return {ResourceAdmissionStatus::Busy, std::nullopt, parseRetryAfter(*body), extractRouterDiagnostic(*body)};
    }
    if (response.statusCode >= 400 && response.statusCode < 500)
    {
        return {ResourceAdmissionStatus::InvalidRequest,
                std::nullopt,
                body.has_value() ? parseRetryAfter(*body) : std::nullopt,
                body.has_value() ? extractRouterDiagnostic(*body) : toBoundedDiagnostic(response.diagnostic)};
    }
    return {ResourceAdmissionStatus::Unavailable,
            std::nullopt,
            body.has_value() ? parseRetryAfter(*body) : std::nullopt,
            body.has_value() ? extractRouterDiagnostic(*body) : toBoundedDiagnostic(response.diagnostic)};
}

// 목적: acquire commit 여부가 불명확해 같은 idempotency request로 확인해야 하는 response 판정
// 입력: response: 첫 POST /v1/leases 결과
// 출력: transport 실패, STORAGE_UNAVAILABLE 또는 식별 불가능한 5xx면 true
[[nodiscard]] bool requiresAcquireOutcomeResolution(const HttpResponse& response)
{
    if (response.statusCode == 0)
    {
        return true;
    }
    if (response.statusCode < 500)
    {
        return false;
    }

    const std::optional<QJsonObject> body = parseJsonObject(response.body);
    if (!body.has_value())
    {
        return true;
    }
    const QString code = body->value(QStringLiteral("code")).toString();
    return code.isEmpty() || code == QStringLiteral("STORAGE_UNAVAILABLE");
}

}  // namespace

// 목적: v1 Resource Router HTTP endpoint를 사용하는 render admission 생성
// 입력: endpoint: Router base URL, timeout: 각 HTTP operation의 bounded wait
// 출력: Qt Network 구현을 public contract 밖에 숨긴 lease adapter
ResourceRouterAdmission::ResourceRouterAdmission(std::string endpoint, const std::chrono::milliseconds timeout)
    : m_endpoint(std::move(endpoint)), m_timeout(timeout)
{}

// 목적: Router POST /v1/leases로 SHARED render claim을 acquire
// 입력: request: v1 idempotency identity, client label, vector와 TTL
// 출력: GRANTED lease 또는 Router BUSY/availability/validation decision
ResourceAdmissionDecision ResourceRouterAdmission::acquire(const ResourceLeaseRequest& request)
{
    if (const std::optional<ResourceAdmissionDecision> validation = validateAcquireRequest(request);
        validation.has_value())
    {
        return *validation;
    }

    const QUrl url = makeRouterUrl(m_endpoint, QStringLiteral("/v1/leases"));
    if (!url.isValid())
    {
        return {ResourceAdmissionStatus::InvalidRequest,
                std::nullopt,
                std::nullopt,
                "Resource Router URL must be an absolute HTTP(S) endpoint."};
    }

    const QJsonObject resources{{QStringLiteral("memory_mb"), static_cast<qint64>(request.claim.memoryMiB)},
                                {QStringLiteral("gpu"), request.claim.requiresGpu},
                                {QStringLiteral("cpu_cores"), request.claim.cpuCores}};
    const QJsonObject payload{{QStringLiteral("request_id"), QString::fromStdString(request.requestId)},
                              {QStringLiteral("client_id"), QString::fromStdString(request.clientId)},
                              {QStringLiteral("mode"), QStringLiteral("SHARED")},
                              {QStringLiteral("resources"), resources},
                              {QStringLiteral("ttl_seconds"), static_cast<qint64>(request.ttl.count())}};
    const QByteArray encodedPayload = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    HttpResponse response = executeRequest(HttpMethod::Post, url, encodedPayload, m_timeout);
    if (requiresAcquireOutcomeResolution(response))
    {
        response = executeRequest(HttpMethod::Post, url, encodedPayload, m_timeout);
    }
    if (response.statusCode != 200)
    {
        return makeFailureDecision(response);
    }

    const std::optional<QJsonObject> body = parseJsonObject(response.body);
    if (!body.has_value() || body->value(QStringLiteral("status")).toString() != QStringLiteral("GRANTED"))
    {
        return {ResourceAdmissionStatus::Unavailable,
                std::nullopt,
                std::nullopt,
                "Resource Router returned a malformed GRANTED lease response."};
    }
    const std::optional<RenderResourceLease> lease = parseLease(*body);
    if (!lease.has_value())
    {
        return {ResourceAdmissionStatus::Unavailable,
                std::nullopt,
                std::nullopt,
                "Resource Router GRANTED response did not contain a valid lease."};
    }
    return {ResourceAdmissionStatus::Granted, *lease, std::nullopt, {}};
}

// 목적: Router POST /v1/leases/{id}/renew로 active lease를 연장
// 입력: lease: active Router lease, ttl: v1 renewal request duration
// 출력: 갱신된 lease 또는 Router terminal/non-ready decision
ResourceAdmissionDecision ResourceRouterAdmission::renew(const RenderResourceLease& lease,
                                                         const std::chrono::seconds ttl)
{
    if (lease.id.empty() || ttl <= std::chrono::seconds::zero())
    {
        return {ResourceAdmissionStatus::InvalidRequest,
                std::nullopt,
                std::nullopt,
                "Resource Router renewal requires a lease ID and positive TTL."};
    }

    const QString encodedLeaseId = QString::fromUtf8(QUrl::toPercentEncoding(QString::fromStdString(lease.id)));
    const QUrl url =
        makeRouterUrl(m_endpoint, QStringLiteral("/v1/leases/") + encodedLeaseId + QStringLiteral("/renew"));
    if (!url.isValid())
    {
        return {ResourceAdmissionStatus::InvalidRequest,
                std::nullopt,
                std::nullopt,
                "Resource Router URL must be an absolute HTTP(S) endpoint."};
    }

    const QJsonObject payload{{QStringLiteral("ttl_seconds"), static_cast<qint64>(ttl.count())}};
    const HttpResponse response =
        executeRequest(HttpMethod::Post, url, QJsonDocument(payload).toJson(QJsonDocument::Compact), m_timeout);
    if (response.statusCode != 200)
    {
        return makeFailureDecision(response);
    }

    const std::optional<QJsonObject> body = parseJsonObject(response.body);
    if (!body.has_value() || body->value(QStringLiteral("status")).toString() != QStringLiteral("ACTIVE"))
    {
        return {ResourceAdmissionStatus::Unavailable,
                std::nullopt,
                std::nullopt,
                "Resource Router returned a malformed lease renewal response."};
    }

    const std::optional<std::chrono::system_clock::time_point> expiry =
        parseExpiry(body->value(QStringLiteral("expires_at")).toString());
    if (!expiry.has_value())
    {
        return {ResourceAdmissionStatus::Unavailable,
                std::nullopt,
                std::nullopt,
                "Resource Router renewal response did not contain a valid expiry."};
    }
    return {ResourceAdmissionStatus::Granted, RenderResourceLease{lease.id, *expiry}, std::nullopt, {}};
}

// 목적: Router DELETE /v1/leases/{id}로 lease를 idempotent release
// 입력: lease: 이전 acquire/renew가 반환한 Router lease
// 출력: 204 acknowledgment 여부와 transport diagnostic
ResourceLeaseReleaseResult ResourceRouterAdmission::release(const RenderResourceLease& lease) noexcept
{
    try
    {
        if (lease.id.empty())
        {
            return {false, "Resource Router release requires a lease ID."};
        }

        const QString encodedLeaseId = QString::fromUtf8(QUrl::toPercentEncoding(QString::fromStdString(lease.id)));
        const QUrl url = makeRouterUrl(m_endpoint, QStringLiteral("/v1/leases/") + encodedLeaseId);
        if (!url.isValid())
        {
            return {false, "Resource Router URL must be an absolute HTTP(S) endpoint."};
        }

        const HttpResponse response = executeRequest(HttpMethod::Delete, url, {}, m_timeout);
        if (response.statusCode == 204)
        {
            return {true, {}};
        }
        const std::optional<QJsonObject> body = parseJsonObject(response.body);
        return {false, body.has_value() ? extractRouterDiagnostic(*body) : toBoundedDiagnostic(response.diagnostic)};
    }
    catch (...)
    {
        return {false, "Resource Router release failed unexpectedly."};
    }
}

}  // namespace flexraw::worker::admission
