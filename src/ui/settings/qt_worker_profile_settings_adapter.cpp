#include "qt_worker_profile_settings_adapter.h"

#include <algorithm>
#include <optional>
#include <unordered_set>

#include <QCoreApplication>
#include <QSettings>
#include <QStringConverter>
#include <QStringList>
#include <QUuid>

namespace flexraw::ui::settings
{
namespace
{

constexpr int WorkerProfileSchemaVersion = 1;
constexpr int DefaultWorkerPort = 47331;

using core::client::ClientError;
using core::client::ClientErrorCode;
using core::client::CreateWorkerProfileCommand;
using core::client::UpdateWorkerProfileCommand;
using core::client::WorkerProfileId;
using core::client::WorkerProfileSnapshot;

// 목적: Worker profile adapter 오류를 공통 frontend error 의미로 생성
// 입력: code: 공통 분류, message: log 전용 진단 text
// 출력: Qt-free ClientError
[[nodiscard]] ClientError makeError(const ClientErrorCode code, std::string message)
{
    return ClientError{code, std::move(message)};
}

// 목적: UTF-8 contract text를 손실 없이 QString으로 변환
// 입력: value: consumer가 전달한 UTF-8 byte sequence
// 출력: 유효한 UTF-8이면 QString, 아니면 nullopt
[[nodiscard]] std::optional<QString> decodeUtf8(const std::string& value)
{
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString decoded = decoder.decode(QByteArray::fromStdString(value));
    return decoder.hasError() ? std::nullopt : std::optional<QString>{decoded};
}

// 목적: profile/storage identity를 brace 없는 canonical UUID text로 정규화
// 입력: value: UUID 후보 text, emptyAllowed: 빈 storage binding 허용 여부
// 출력: canonical UUID, 허용된 빈 값 또는 invalid면 nullopt
[[nodiscard]] std::optional<QString> normalizedUuid(const QString& value, const bool emptyAllowed)
{
    const QString trimmed = value.trimmed();
    if (emptyAllowed && trimmed.isEmpty())
    {
        return QString{};
    }
    const QUuid uuid = QUuid::fromString(trimmed);
    return uuid.isNull() ? std::nullopt : std::optional<QString>{uuid.toString(QUuid::WithoutBraces)};
}

// 목적: Qt-free profile command fields를 저장 가능한 normalized snapshot으로 검증
// 입력: id와 command field 전체
// 출력: normalized profile 또는 validation 오류
template<typename Command>
[[nodiscard]] core::client::WorkerProfileResult normalizedProfile(const WorkerProfileId& id, const Command& command)
{
    const std::optional<QString> profileId = normalizedUuid(QString::fromStdString(id.value), false);
    const std::optional<QString> displayName = decodeUtf8(command.displayName);
    const std::optional<QString> host = decodeUtf8(command.host);
    const std::optional<QString> sourceStorage = decodeUtf8(command.expectedSourceStorageId);
    const std::optional<QString> outputStorage = decodeUtf8(command.expectedOutputStorageId);
    if (!profileId.has_value() || !displayName.has_value() || !host.has_value() || !sourceStorage.has_value() ||
        !outputStorage.has_value())
    {
        return core::client::WorkerProfileResult::failure(
            makeError(ClientErrorCode::InvalidArgument, "Worker profile contains invalid UTF-8 or identity."));
    }

    const QString normalizedName = displayName->trimmed();
    const QString normalizedHost = host->trimmed();
    const std::optional<QString> normalizedSource = normalizedUuid(*sourceStorage, true);
    const std::optional<QString> normalizedOutput = normalizedUuid(*outputStorage, true);
    if (normalizedName.isEmpty() || normalizedHost.isEmpty() || command.port == 0 || !normalizedSource.has_value() ||
        !normalizedOutput.has_value())
    {
        return core::client::WorkerProfileResult::failure(
            makeError(ClientErrorCode::InvalidArgument, "Worker profile fields are invalid."));
    }

    return core::client::WorkerProfileResult::success(WorkerProfileSnapshot{
        WorkerProfileId{profileId->toStdString()},
        normalizedName.toUtf8().toStdString(),
        normalizedHost.toUtf8().toStdString(),
        command.port,
        command.enabled,
        normalizedSource->toStdString(),
        normalizedOutput->toStdString(),
    });
}

// 목적: profile snapshot 하나를 현재 QSettings schema에 기록
// 입력: settings: application store, profile: normalized snapshot
// 출력: 없음
void writeProfile(QSettings& settings, const WorkerProfileSnapshot& profile)
{
    settings.beginGroup(QStringLiteral("workerProfiles/profiles/%1").arg(QString::fromStdString(profile.id.value)));
    settings.setValue(QStringLiteral("displayName"), QString::fromUtf8(profile.displayName.c_str()));
    settings.setValue(QStringLiteral("host"), QString::fromUtf8(profile.host.c_str()));
    settings.setValue(QStringLiteral("port"), profile.port);
    settings.setValue(QStringLiteral("enabled"), profile.enabled);
    settings.setValue(QStringLiteral("expectedSourceStorageId"),
                      QString::fromStdString(profile.expectedSourceStorageId));
    settings.setValue(QStringLiteral("expectedOutputStorageId"),
                      QString::fromStdString(profile.expectedOutputStorageId));
    settings.endGroup();
}

// 목적: legacy export endpoint를 최초 Worker profile schema로 한 번 migration
// 입력: settings: 기존 export key와 새 schema를 공유하는 application store
// 출력: migration 성공이면 true
[[nodiscard]] bool migrateLegacySettings(QSettings& settings)
{
    settings.beginGroup(QStringLiteral("export"));
    const QString host = settings.value(QStringLiteral("remoteHost"), QStringLiteral("127.0.0.1")).toString().trimmed();
    bool portConverted = false;
    const int storedPort = settings.value(QStringLiteral("remotePort"), DefaultWorkerPort).toInt(&portConverted);
    const int port = portConverted && storedPort >= 1 && storedPort <= 65535 ? storedPort : DefaultWorkerPort;
    const QString sourceStorage =
        normalizedUuid(settings.value(QStringLiteral("expectedSourceStorageId")).toString(), true).value_or(QString{});
    const QString outputStorage =
        normalizedUuid(settings.value(QStringLiteral("expectedOutputStorageId")).toString(), true).value_or(QString{});
    settings.endGroup();

    const QString profileId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString displayName = QCoreApplication::translate("WorkerProfiles", "Default Worker");
    writeProfile(settings,
                 WorkerProfileSnapshot{WorkerProfileId{profileId.toStdString()},
                                       displayName.toUtf8().toStdString(),
                                       host.isEmpty() ? std::string{"127.0.0.1"} : host.toUtf8().toStdString(),
                                       static_cast<std::uint16_t>(port),
                                       true,
                                       sourceStorage.toStdString(),
                                       outputStorage.toStdString()});
    settings.setValue(QStringLiteral("workerProfiles/order"), QStringList{profileId});
    settings.setValue(QStringLiteral("workerProfiles/schemaVersion"), WorkerProfileSchemaVersion);
    settings.setValue(QStringLiteral("export/preferredWorkerProfileId"), profileId);
    settings.remove(QStringLiteral("export/remoteHost"));
    settings.remove(QStringLiteral("export/remotePort"));
    settings.remove(QStringLiteral("export/expectedSourceStorageId"));
    settings.remove(QStringLiteral("export/expectedOutputStorageId"));
    settings.remove(QStringLiteral("export/localSourceRoot"));
    settings.remove(QStringLiteral("export/localOutputRoot"));
    settings.sync();
    return settings.status() == QSettings::NoError;
}

// 목적: Worker profile schema 존재와 지원 version 보장
// 입력: settings: application store
// 출력: 사용 가능하면 nullopt, 아니면 저장소 오류
[[nodiscard]] std::optional<ClientError> ensureSchema(QSettings& settings)
{
    if (!settings.contains(QStringLiteral("workerProfiles/schemaVersion")))
    {
        if (!migrateLegacySettings(settings))
        {
            return makeError(ClientErrorCode::DatabaseError, "Unable to migrate Worker profile settings.");
        }
        return std::nullopt;
    }
    bool converted = false;
    const int version = settings.value(QStringLiteral("workerProfiles/schemaVersion")).toInt(&converted);
    if (!converted || version != WorkerProfileSchemaVersion)
    {
        return makeError(ClientErrorCode::DatabaseError, "Worker profile settings schema is unsupported.");
    }
    return std::nullopt;
}

// 목적: 저장된 profile group 하나를 Qt-free snapshot으로 엄격하게 복원
// 입력: settings: application store, profileId: canonical UUID text
// 출력: normalized snapshot 또는 손상된 저장소 오류
[[nodiscard]] core::client::WorkerProfileResult readProfile(QSettings& settings, const QString& profileId)
{
    const QString group = QStringLiteral("workerProfiles/profiles/%1").arg(profileId);
    settings.beginGroup(group);
    const bool complete = settings.contains(QStringLiteral("displayName")) &&
                          settings.contains(QStringLiteral("host")) && settings.contains(QStringLiteral("port")) &&
                          settings.contains(QStringLiteral("enabled"));
    bool portConverted = false;
    const int port = settings.value(QStringLiteral("port")).toInt(&portConverted);
    const UpdateWorkerProfileCommand command{
        WorkerProfileId{profileId.toStdString()},
        settings.value(QStringLiteral("displayName")).toString().toUtf8().toStdString(),
        settings.value(QStringLiteral("host")).toString().toUtf8().toStdString(),
        portConverted && port >= 1 && port <= 65535 ? static_cast<std::uint16_t>(port) : std::uint16_t{0},
        settings.value(QStringLiteral("enabled")).toBool(),
        settings.value(QStringLiteral("expectedSourceStorageId")).toString().toStdString(),
        settings.value(QStringLiteral("expectedOutputStorageId")).toString().toStdString(),
    };
    settings.endGroup();
    if (!complete)
    {
        return core::client::WorkerProfileResult::failure(
            makeError(ClientErrorCode::DatabaseError, "Worker profile record is incomplete."));
    }
    core::client::WorkerProfileResult result = normalizedProfile(command.id, command);
    if (result.hasError())
    {
        return core::client::WorkerProfileResult::failure(
            makeError(ClientErrorCode::DatabaseError, "Worker profile record is invalid."));
    }
    return result;
}

// 목적: profile 이름의 application-level case-insensitive uniqueness 확인
// 입력: profiles: 현재 snapshot, candidate: 새 이름, ignoredId: update 대상 identity
// 출력: 충돌하면 true
[[nodiscard]] bool hasDuplicateName(const std::vector<WorkerProfileSnapshot>& profiles,
                                    const std::string& candidate,
                                    const std::optional<WorkerProfileId>& ignoredId)
{
    const QString candidateName = QString::fromUtf8(candidate.c_str()).trimmed();
    return std::ranges::any_of(profiles, [&](const WorkerProfileSnapshot& profile) {
        return (!ignoredId.has_value() || profile.id != *ignoredId) &&
               QString::fromUtf8(profile.displayName.c_str()).compare(candidateName, Qt::CaseInsensitive) == 0;
    });
}

}  // namespace

// 목적: application settings를 Worker profile contract 저장소로 연결
// 입력: settings: adapter보다 오래 유지되는 application QSettings
// 출력: lazy schema migration이 가능한 profile client
QtWorkerProfileSettingsAdapter::QtWorkerProfileSettingsAdapter(QSettings& settings) : m_settings(settings) {}

// 목적: 저장 순서가 보존된 현재 Worker profile snapshot 목록 조회
// 입력: 없음
// 출력: profile 목록 또는 schema·저장소 오류
core::client::WorkerProfileListResult QtWorkerProfileSettingsAdapter::listWorkerProfiles()
{
    if (const std::optional<ClientError> error = ensureSchema(m_settings); error.has_value())
    {
        return core::client::WorkerProfileListResult::failure(*error);
    }

    const QStringList order = m_settings.value(QStringLiteral("workerProfiles/order")).toStringList();
    m_settings.beginGroup(QStringLiteral("workerProfiles/profiles"));
    const QStringList storedGroups = m_settings.childGroups();
    m_settings.endGroup();
    if (order.size() != storedGroups.size())
    {
        return core::client::WorkerProfileListResult::failure(
            makeError(ClientErrorCode::DatabaseError, "Worker profile order does not match stored records."));
    }

    std::unordered_set<std::string> seen;
    std::vector<WorkerProfileSnapshot> profiles;
    profiles.reserve(static_cast<std::size_t>(order.size()));
    for (const QString& rawId : order)
    {
        const std::optional<QString> id = normalizedUuid(rawId, false);
        if (!id.has_value() || !seen.insert(id->toStdString()).second || !storedGroups.contains(*id))
        {
            return core::client::WorkerProfileListResult::failure(
                makeError(ClientErrorCode::DatabaseError, "Worker profile identity index is invalid."));
        }
        core::client::WorkerProfileResult profile = readProfile(m_settings, *id);
        if (profile.hasError())
        {
            return core::client::WorkerProfileListResult::failure(profile.error());
        }
        profiles.push_back(profile.value());
    }
    return core::client::WorkerProfileListResult::success(std::move(profiles));
}

// 목적: 검증된 Worker profile을 새 UUID identity로 저장
// 입력: command: 이름, endpoint, 활성화 상태와 expected storage binding
// 출력: 생성된 normalized profile 또는 validation·저장소 오류
core::client::WorkerProfileResult QtWorkerProfileSettingsAdapter::createWorkerProfile(
    const CreateWorkerProfileCommand& command)
{
    const WorkerProfileId id{QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString()};
    core::client::WorkerProfileResult normalized = normalizedProfile(id, command);
    if (normalized.hasError())
    {
        return normalized;
    }
    const core::client::WorkerProfileListResult existing = listWorkerProfiles();
    if (existing.hasError())
    {
        return core::client::WorkerProfileResult::failure(existing.error());
    }
    if (hasDuplicateName(existing.value(), normalized.value().displayName, std::nullopt))
    {
        return core::client::WorkerProfileResult::failure(
            makeError(ClientErrorCode::Conflict, "Worker profile display name already exists."));
    }

    writeProfile(m_settings, normalized.value());
    QStringList order = m_settings.value(QStringLiteral("workerProfiles/order")).toStringList();
    order.push_back(QString::fromStdString(normalized.value().id.value));
    m_settings.setValue(QStringLiteral("workerProfiles/order"), order);
    m_settings.sync();
    return m_settings.status() == QSettings::NoError
               ? normalized
               : core::client::WorkerProfileResult::failure(
                     makeError(ClientErrorCode::DatabaseError, "Unable to save Worker profile."));
}

// 목적: stable identity를 유지하며 검증된 Worker profile 전체 갱신
// 입력: command: 대상 identity와 교체할 profile 속성
// 출력: 갱신된 normalized profile 또는 validation·not-found·저장소 오류
core::client::WorkerProfileResult QtWorkerProfileSettingsAdapter::updateWorkerProfile(
    const UpdateWorkerProfileCommand& command)
{
    core::client::WorkerProfileResult normalized = normalizedProfile(command.id, command);
    if (normalized.hasError())
    {
        return normalized;
    }
    const core::client::WorkerProfileListResult existing = listWorkerProfiles();
    if (existing.hasError())
    {
        return core::client::WorkerProfileResult::failure(existing.error());
    }
    const auto match = std::ranges::find(existing.value(), command.id, &WorkerProfileSnapshot::id);
    if (match == existing.value().end())
    {
        return core::client::WorkerProfileResult::failure(
            makeError(ClientErrorCode::NotFound, "Worker profile was not found."));
    }
    if (hasDuplicateName(existing.value(), normalized.value().displayName, command.id))
    {
        return core::client::WorkerProfileResult::failure(
            makeError(ClientErrorCode::Conflict, "Worker profile display name already exists."));
    }

    writeProfile(m_settings, normalized.value());
    m_settings.sync();
    return m_settings.status() == QSettings::NoError
               ? normalized
               : core::client::WorkerProfileResult::failure(
                     makeError(ClientErrorCode::DatabaseError, "Unable to update Worker profile."));
}

// 목적: Worker profile과 저장 순서 entry 제거
// 입력: id: 제거할 stable profile identity
// 출력: 제거된 identity 또는 validation·not-found·저장소 오류
core::client::WorkerProfileRemoveResult QtWorkerProfileSettingsAdapter::removeWorkerProfile(const WorkerProfileId& id)
{
    const std::optional<QString> normalizedId = normalizedUuid(QString::fromStdString(id.value), false);
    if (!normalizedId.has_value())
    {
        return core::client::WorkerProfileRemoveResult::failure(
            makeError(ClientErrorCode::InvalidArgument, "Worker profile identity is invalid."));
    }
    const core::client::WorkerProfileListResult existing = listWorkerProfiles();
    if (existing.hasError())
    {
        return core::client::WorkerProfileRemoveResult::failure(existing.error());
    }
    if (std::ranges::find(existing.value(), id, &WorkerProfileSnapshot::id) == existing.value().end())
    {
        return core::client::WorkerProfileRemoveResult::failure(
            makeError(ClientErrorCode::NotFound, "Worker profile was not found."));
    }

    m_settings.remove(QStringLiteral("workerProfiles/profiles/%1").arg(*normalizedId));
    QStringList order = m_settings.value(QStringLiteral("workerProfiles/order")).toStringList();
    order.removeAll(*normalizedId);
    m_settings.setValue(QStringLiteral("workerProfiles/order"), order);
    m_settings.sync();
    return m_settings.status() == QSettings::NoError
               ? core::client::WorkerProfileRemoveResult::success(id)
               : core::client::WorkerProfileRemoveResult::failure(
                     makeError(ClientErrorCode::DatabaseError, "Unable to remove Worker profile."));
}

// 목적: 다른 profile 속성을 유지하며 enabled 정책 갱신
// 입력: command: 대상 identity와 새 enabled 상태
// 출력: 갱신된 profile 또는 validation·not-found·저장소 오류
core::client::WorkerProfileResult QtWorkerProfileSettingsAdapter::setWorkerProfileEnabled(
    const core::client::SetWorkerProfileEnabledCommand& command)
{
    const core::client::WorkerProfileListResult existing = listWorkerProfiles();
    if (existing.hasError())
    {
        return core::client::WorkerProfileResult::failure(existing.error());
    }
    const auto match = std::ranges::find(existing.value(), command.id, &WorkerProfileSnapshot::id);
    if (match == existing.value().end())
    {
        return core::client::WorkerProfileResult::failure(
            makeError(ClientErrorCode::NotFound, "Worker profile was not found."));
    }
    return updateWorkerProfile(UpdateWorkerProfileCommand{match->id,
                                                          match->displayName,
                                                          match->host,
                                                          match->port,
                                                          command.enabled,
                                                          match->expectedSourceStorageId,
                                                          match->expectedOutputStorageId});
}

}  // namespace flexraw::ui::settings
