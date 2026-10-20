#pragma once
#include "languageprofile.h"
#include <QVariantMap>
namespace swipekb {
// Values only; no engine/editor/policy object or text is accepted by this shared interface.
QVariantMap profileDiagnostics(const ProfileSnapshot &profile);
}
