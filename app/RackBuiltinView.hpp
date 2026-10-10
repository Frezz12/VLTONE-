#pragma once
#include <QString>
#include <QWidget>
class RackParameterBinding;

/// Compact, host-drawn faces. Full editors and DSP stay independent of layout.
class RackBuiltinView final : public QWidget {
    Q_OBJECT
  public:
    RackBuiltinView(RackParameterBinding*, QString uid, QWidget* parent = nullptr);
    static int preferredWidth(const QString& uid);
    static bool supports(const QString& uid);
    void refresh();

  private:
    RackParameterBinding* m_binding;
    QString m_uid;
    class Impl;
    Impl* m_impl;
};
