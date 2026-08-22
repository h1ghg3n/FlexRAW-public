#pragma once

#include <QString>

#include "develop_params.h"

namespace flexraw::core::preset
{

struct PresetDefinition
{
    QString name;
    QString category;
    types::DevelopParams params;
};

}  // namespace flexraw::core::preset
