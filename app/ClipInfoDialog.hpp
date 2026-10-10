#pragma once

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace ui {

// Read-only, copyable details. Long names and paths scroll within their row
// instead of expanding this utility window beyond the screen.
class ClipInfoDialog final : public QDialog {
public:
    ClipInfoDialog(const QString& title, const QString& name,
                   const QString& closeText, QWidget* parent)
        : QDialog(parent, Qt::Tool) {
        setObjectName(QStringLiteral("ClipInformationDialog"));
        setWindowTitle(title);
        setAttribute(Qt::WA_DeleteOnClose);
        setModal(false);
        setStyleSheet(QStringLiteral(
            "QDialog#ClipInformationDialog QLineEdit { background: transparent;"
            " border: none; border-bottom: 1px solid transparent; border-radius: 0;"
            " padding: 1px 0; }"
            "QDialog#ClipInformationDialog QLineEdit:focus {"
            " border-bottom-color: palette(highlight); }"));
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(20, 16, 20, 16);
        layout->setSpacing(14);
        auto* heading = new QLineEdit(name, this);
        heading->setReadOnly(true);
        heading->setFrame(false);
        heading->setAccessibleName(title);
        heading->setToolTip(name);
        QFont font = heading->font();
        font.setBold(true);
        heading->setFont(font);
        heading->setCursorPosition(0);
        layout->addWidget(heading);
        m_form = new QFormLayout;
        m_form->setHorizontalSpacing(20);
        m_form->setVerticalSpacing(4);
        m_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        layout->addLayout(m_form);
        auto* buttons = new QDialogButtonBox(this);
        auto* close = buttons->addButton(closeText, QDialogButtonBox::RejectRole);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
        close->setDefault(true);
        close->setFocus();
        resize(460, sizeHint().height());
    }

    void addDetail(const QString& id, const QString& label, const QString& value) {
        auto* field = new QLineEdit(this);
        field->setObjectName(id);
        field->setReadOnly(true);
        field->setFrame(false);
        field->setMinimumWidth(0);
        field->setAccessibleName(label);
        auto* caption = new QLabel(label, this);
        caption->setTextFormat(Qt::PlainText);
        caption->setBuddy(field);
        m_form->addRow(caption, field);
        setDetail(id, value);
    }

    void setDetail(const QString& id, const QString& value) {
        if (auto* field = findChild<QLineEdit*>(id)) {
            field->setText(value);
            field->setCursorPosition(0);
            field->setToolTip(value);
        }
    }

private:
    QFormLayout* m_form = nullptr;
};

} // namespace ui
