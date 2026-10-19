#pragma once

#include "product_export_client_adapter.h"

namespace flexraw::ui::export_
{

// 기존 Qt UI consumer source compatibility용 이름입니다. 구현과 lifecycle projection은 Product Runtime target에
// 있습니다.
using QtExportClientAdapter = runtime::ProductExportClientAdapter;

}  // namespace flexraw::ui::export_
