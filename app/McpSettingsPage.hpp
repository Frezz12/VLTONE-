#pragma once
#include <QWidget>
namespace ui {
class McpSettingsPage final : public QWidget {
    Q_OBJECT
public:
    explicit McpSettingsPage(QWidget *parent=nullptr);
};
}
