#pragma once

#include <QRectF>
#include <QString>

class QPainter;

namespace ui {
enum class KnobDesign { Classic, BrushedMetal };

KnobDesign knobDesign();
void setKnobDesign(KnobDesign design);

/// Paint just the body and its position marker, preserving the caller's scale,
/// labels and modulation ring. Angles follow QPainter's arc convention.
/// Returns false for the classic design so each editor retains its own face.
bool paintMetalKnob(QPainter& painter, const QRectF& body, double angleDegrees);

bool checkKnobAppearanceForTest(const QString& screenshotDirectory = {});
}
