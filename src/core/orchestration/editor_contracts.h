#pragma once

#include <optional>

#include <QMetaType>

#include "catalog_entry.h"
#include "develop_params.h"
#include "error.h"
#include "file_types.h"
#include "preview_contracts.h"
#include "result.h"

namespace flexraw::core::orchestration
{

struct SourceResolutionCapabilities
{
    bool canAcceptReplacement{false};
    bool canRegisterReplacementAsNew{false};
    bool canRelinkSource{false};
};

struct EditorState
{
    bool hasSelection{false};
    PhotoSnapshot photo;
    types::DevelopRevision persistedRevision{0};
    types::FileDescriptor source;
    types::DevelopParams params;
    bool sourceProcessingAllowed{false};
    std::optional<catalog::SourceBindingState> sourceState;
    SourceResolutionCapabilities sourceResolution;
    bool dirty{false};
    bool canUndo{false};
    bool canRedo{false};
};

using EditorStateResult = types::Result<EditorState, types::CoreError>;

}  // namespace flexraw::core::orchestration

Q_DECLARE_METATYPE(flexraw::core::orchestration::EditorState)
