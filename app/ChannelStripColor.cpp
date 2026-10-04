#include "ChannelStrip.hpp"
#include "EngineController.hpp"
#include "MiniModuleRack.hpp"
#include "MixerPreferences.hpp"
#include <QLayout>
#include <algorithm>

ChannelStrip::~ChannelStrip() {
  if (auto *rack = qobject_cast<ui::MiniModuleRack *>(m_colorWell))
    rack->finishEdits();
}
QWidget *ChannelStrip::buildColor() {
  const auto *track =
      m_controller->project().findTrack(m_trackId.toStdString());
  if (!m_master && (!track || !daw::carriesAudio(*track)))
    return nullptr;
  auto *rack = new ui::MiniModuleRack(
      m_controller,
      m_master ? QString::fromUtf8(daw::EngineController::kMasterChannelId)
               : m_trackId,
      this);
  m_colorWell = rack;
  rack->setStripWidth(m_stripWidth);
  connect(rack, &ui::MiniModuleRack::edited, this, &ChannelStrip::edited);
  connect(rack, &ui::MiniModuleRack::automateRequested, this,
          [this](const QString &id, const QString &parameter) {
            emit automatePluginRequested(
                m_master
                    ? QString::fromUtf8(daw::EngineController::kMasterChannelId)
                    : m_trackId,
                id, parameter);
          });
  connect(rack, &ui::MiniModuleRack::layoutChanged, this,
          &ChannelStrip::refreshColorRackRow);
  connect(&ui::MixerPreferences::instance(),
          &ui::MixerPreferences::colorVisibleChanged, this,
          [this](bool) { refreshColorRackRow(); });
  return rack;
}

void ChannelStrip::refreshColorRackRow() {
  if (!m_rackSections[1])
    return;
  const bool shown =
      m_colorWell && ui::MixerPreferences::instance().colorVisible();
  if (m_colorWell) {
    m_colorWell->setVisible(shown);
    m_colorWell->layout()->activate();
  }
  m_rackSections[1]->layout()->invalidate();
  const auto *rack = qobject_cast<ui::MiniModuleRack *>(m_colorWell);
  const int newHeight =
      shown ? (rack ? rack->naturalHeight() : m_colorWell->height()) : 0;
  if (m_rackSections[1]->height() == newHeight &&
      m_rackNaturalHeights[1] == newHeight)
    return;
  m_rackNaturalHeights[1] = newHeight;
  m_rackSections[1]->setFixedHeight(newHeight);
  m_rackSections[1]->setVisible(newHeight > 0);
  m_rack->layout()->invalidate();
  m_rackNaturalHeight = 0;
  for (const int height : m_rackNaturalHeights)
    m_rackNaturalHeight += height;
  const int visibleRows = int(std::count_if(m_rackNaturalHeights.begin(),
                                            m_rackNaturalHeights.end(),
                                            [](int h) { return h > 0; }));
  m_rackNaturalHeight += std::max(0, visibleRows - 1) * 6;
  m_rackExtraHeight = m_rack->sizeHint().height() - m_rackNaturalHeight;
  m_rack->setFixedHeight(m_rackNaturalHeight + m_rackExtraHeight);
  m_layoutWidth = 0;
  updateResponsiveLayout();
  emit rackLayoutChanged();
}

void ChannelStrip::syncColor(bool automation) {
  if (auto *rack = qobject_cast<ui::MiniModuleRack *>(m_colorWell))
    rack->sync(automation);
}
