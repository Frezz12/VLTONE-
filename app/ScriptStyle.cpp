#include "ScriptStyle.hpp"
#include "Controls.hpp"
#include "CreatorStyle.hpp"
#include "Theme.hpp"
#include <QColor>
#include <QHash>
#include <QLayout>
#include <QRegularExpression>
#include <QVariant>
#include <QWidget>
#include <cmath>

namespace ui::scriptstyle {
namespace {
const QStringList colors{"background", "surface", "field",    "text",
                         "muted",      "accent",  "onAccent", "border"};
const QStringList paintFields{"background", "color", "accent", "borderColor"};
const QStringList states{"hover", "pressed", "checked", "disabled", "focus"};
QString text(const Json &value) { return QString::fromStdString(value.get<std::string>()); }
void color(const Json &value, bool tokens) {
    if (!value.is_string())
        throw std::runtime_error("Style color must be text");
    const auto name = text(value);
    if (tokens && name.startsWith('$') && colors.contains(name.mid(1)))
        return;
    if (name.size() > 64 || !QColor::isValidColorName(name))
        throw std::runtime_error("Invalid style color: " + value.get<std::string>());
}
void validate(const Json &style, bool state = false) {
    if (!style.is_object() || style.size() > 24)
        throw std::runtime_error("Style must be an object with at most 24 properties");
    for (const auto &[key, value] : style.items()) {
        const auto field = QString::fromStdString(key);
        if (paintFields.contains(field) && (!state || key != "accent"))
            color(value, true);
        else if (!state && states.contains(field))
            validate(value, true);
        else if (!state && key == "fontFamily") {
            static const QRegularExpression family(QStringLiteral("^[\\p{L}\\p{N} _.-]{1,80}$"));
            if (!value.is_string() || !family.match(text(value)).hasMatch())
                throw std::runtime_error("fontFamily must name an installed font");
        } else if (!state && (key == "fontSize" || key == "fontWeight" || key == "radius" ||
                              key == "borderWidth" || key == "padding")) {
            const double low = key == "fontSize" ? 10 : key == "fontWeight" ? 100 : 0;
            const double high = key == "fontWeight" ? 900 : key == "borderWidth" ? 12 : 96;
            if (!value.is_number_integer() || value.get<double>() < low ||
                value.get<double>() > high || (key == "fontWeight" && value.get<int>() % 100))
                throw std::runtime_error("Invalid style " + key);
        } else
            throw std::runtime_error("Unknown style property: " + key);
    }
}
QHash<QString, QColor> paletteTokens(const CreatorColors &c) {
    return {{"background", c.panel},  {"surface", c.card}, {"field", c.field},
            {"text", c.text},         {"muted", c.muted},  {"accent", c.accent},
            {"onAccent", c.onAccent}, {"border", c.border}};
}
QColor resolvedColor(const Json &value, const QHash<QString, QColor> &tokens) {
    const auto name = text(value);
    return name.startsWith('$') ? tokens.value(name.mid(1)) : QColor(name);
}
QString cssColor(const QColor &color) {
    return QString("rgba(%1,%2,%3,%4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}
QString declarations(const Json &style, const QHash<QString, QColor> &tokens, bool layout) {
    QString css;
    for (const auto &[key, value] : style.items()) {
        const auto field = QString::fromStdString(key);
        if (paintFields.contains(field)) {
            if (key == "accent")
                continue;
            css += (key == "background"    ? "background-color"
                    : key == "borderColor" ? "border-color"
                                           : "color") +
                   QString(":%1;").arg(cssColor(resolvedColor(value, tokens)));
        } else if (key == "radius")
            css += QString("border-radius:%1px;").arg(value.get<int>());
        else if (key == "borderWidth")
            css += QString("border:%1px solid;").arg(value.get<int>());
        else if (key == "padding" && !layout)
            css += QString("padding:%1px;").arg(value.get<int>());
        else if (key == "fontSize")
            css += QString("font-size:%1px;").arg(value.get<int>());
        else if (key == "fontWeight")
            css += QString("font-weight:%1;").arg(value.get<int>());
        else if (key == "fontFamily")
            css += QString("font-family:'%1';").arg(text(value));
    }
    // Set border colour after the shorthand, regardless of JSON key ordering.
    if (style.contains("borderColor"))
        css += "border-color:" + cssColor(resolvedColor(style["borderColor"], tokens)) + ";";
    return css;
}
void apply(QWidget *widget, const QHash<QString, QColor> &tokens) {
    const auto style = read(widget);
    // QSS can temporarily replace QWidget palettes during polish. Keep the
    // authored palette separately, including through unstyled label wrappers.
    auto palette = widget->palette();
    palette.setColor(QPalette::Window, tokens["background"]);
    palette.setColor(QPalette::Base, tokens["field"]);
    palette.setColor(QPalette::Button, tokens["surface"]);
    palette.setColor(QPalette::Text, tokens["text"]);
    palette.setColor(QPalette::WindowText, tokens["text"]);
    palette.setColor(QPalette::ButtonText, tokens["text"]);
    palette.setColor(QPalette::Highlight, tokens["accent"]);
    palette.setColor(QPalette::HighlightedText, tokens["onAccent"]);
    palette.setColor(QPalette::PlaceholderText, tokens["muted"]);
    for (auto *parent = widget->parentWidget(); parent; parent = parent->parentWidget())
        if (parent->property("vlt.scriptPaletteData").isValid()) {
            palette = parent->property("vlt.scriptPaletteData").value<QPalette>();
            break;
        }
    if (style.contains("color")) {
        const auto ink = resolvedColor(style["color"], tokens);
        for (auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText})
            palette.setColor(role, ink);
    }
    if (style.contains("background")) {
        const auto fill = resolvedColor(style["background"], tokens);
        for (auto role : {QPalette::Window, QPalette::Base, QPalette::Button})
            palette.setColor(role, fill);
    }
    if (style.contains("accent"))
        palette.setColor(QPalette::Highlight, resolvedColor(style["accent"], tokens));
    widget->setProperty("vlt.scriptPalette", true);
    widget->setProperty("vlt.scriptPaletteData", QVariant::fromValue(palette));
    // Bound sliders are a native field containing the actual slider and readout.
    if (widget->property("vlt.type").toString() == "slider")
        for (auto *slider : widget->findChildren<QSlider *>()) {
            slider->setProperty("vlt.scriptPalette", true);
            slider->setProperty("vlt.scriptPaletteData", QVariant::fromValue(palette));
            slider->setPalette(palette);
        }
    if (auto *knob = qobject_cast<Knob *>(widget))
        knob->setArcColor(palette.highlight().color());
    const auto selector =
        QString("*[vltStyleId=\"%1\"]").arg(QString::number(quintptr(widget), 16));
    widget->setProperty("vltStyleId", QString::number(quintptr(widget), 16));
    widget->setAttribute(Qt::WA_StyledBackground,
                         style.contains("background") || style.contains("borderWidth"));
    QString css = selector + "{" + declarations(style, tokens, widget->layout()) + "}";
    Json inherited = Json::object();
    for (const char *key : {"color", "fontFamily", "fontSize", "fontWeight"})
        if (style.contains(key))
            inherited[key] = style[key];
    if (!inherited.empty()) {
        css += selector + " QWidget{" + declarations(inherited, tokens, false) + "}";
        css += selector + " QWidget:disabled{color:" + cssColor(tokens["muted"]) + ";}";
    }
    for (const auto &state : states)
        if (style.contains(state.toStdString()))
            css += selector + ":" + state + "{" +
                   declarations(style[state.toStdString()], tokens, false) + "}";
    // Foreground and geometry stay readable when a custom button is disabled.
    if (!style.contains("disabled"))
        css += selector + ":disabled{color:" + cssColor(tokens["muted"]) + ";}";
    const auto localCss = style.empty() ? QString() : css;
    if (widget->styleSheet() != localCss)
        widget->setStyleSheet(localCss);
    if (auto *layout = widget->layout()) {
        const auto original = widget->property("vlt.styleMargins");
        if (!original.isValid()) {
            const auto margins = layout->contentsMargins();
            widget->setProperty(
                "vlt.styleMargins",
                QVariantList{margins.left(), margins.top(), margins.right(), margins.bottom()});
        }
        const auto margins = widget->property("vlt.styleMargins").toList();
        if (style.contains("padding")) {
            const int padding = style["padding"];
            layout->setContentsMargins(padding, padding, padding, padding);
        } else
            layout->setContentsMargins(margins[0].toInt(), margins[1].toInt(), margins[2].toInt(),
                                       margins[3].toInt());
    }
    // Qt's stylesheet polish restores cached minimum sizes. Explicit Lua layout
    // constraints must win, including when a local style is removed.
    widget->ensurePolished();
    widget->setPalette(palette);
    if (widget->property("vlt.minWidth").isValid())
        widget->setMinimumWidth(widget->property("vlt.minWidth").toInt());
    if (widget->property("vlt.minHeight").isValid())
        widget->setMinimumHeight(widget->property("vlt.minHeight").toInt());
    if (widget->property("vlt.fixedWidth").isValid())
        widget->setFixedWidth(widget->property("vlt.fixedWidth").toInt());
    if (widget->property("vlt.fixedHeight").isValid())
        widget->setFixedHeight(widget->property("vlt.fixedHeight").toInt());
    widget->update();
}
} // namespace
void validateTheme(const Json &theme) {
    if (!theme.is_object() || theme.size() > size_t(colors.size()))
        throw std::runtime_error("Invalid window theme");
    for (const auto &[key, value] : theme.items()) {
        if (!colors.contains(QString::fromStdString(key)))
            throw std::runtime_error("Unknown theme color: " + key);
        color(value, false);
        if (QColor(text(value)).alpha() != 255)
            throw std::runtime_error("Window theme colors must be opaque");
    }
}
void validateDefinitions(const Json &styles) {
    if (!styles.is_object() || styles.size() > 64)
        throw std::runtime_error("At most 64 named styles per window");
    for (const auto &[name, style] : styles.items()) {
        if (name.empty() || name.size() > 80)
            throw std::runtime_error("Invalid style name");
        validate(style);
    }
}
Json resolve(const Json &style, const Json &styles) {
    Json result = Json::object();
    const auto reference = style.is_string()   ? style
                           : style.is_object() ? style.value("use", Json())
                                               : Json();
    if (!reference.is_null()) {
        if (!reference.is_string() || !styles.contains(reference.get<std::string>()))
            throw std::runtime_error("Unknown named style");
        result = styles.at(reference.get<std::string>());
    }
    if (style.is_object()) {
        auto overrides = style;
        overrides.erase("use");
        result.update(overrides);
    } else if (!style.is_string())
        throw std::runtime_error("style must be a name or an object");
    validate(result);
    return result;
}
void remember(QWidget *widget, const Json &resolved) {
    widget->setProperty("vlt.style", QByteArray::fromStdString(resolved.dump()));
}
Json read(const QWidget *widget) {
    const auto value = widget->property("vlt.style").toByteArray();
    return value.isEmpty() ? Json::object() : Json::parse(value.toStdString());
}
void applyWindow(QWidget *window, QWidget *content) {
    auto c = creatorColors();
    const auto savedTheme = content->property("vlt.theme").toByteArray();
    const auto theme =
        savedTheme.isEmpty() ? Json::object() : Json::parse(savedTheme.toStdString());
    const auto set = [&](const char *key, QColor &value) {
        if (theme.contains(key))
            value = QColor(text(theme[key]));
    };
    set("background", c.panel);
    set("surface", c.card);
    set("field", c.field);
    set("text", c.text);
    set("muted", c.muted);
    set("accent", c.accent);
    set("border", c.border);
    c.accentFill = c.accent;
    if (theme.contains("accent") && !theme.contains("onAccent"))
        c.onAccent = contrastingTextColor(c.accent);
    set("onAccent", c.onAccent);
    c.hover = mixColors(c.card, c.text, .08);
    styleCreator(window, c);
    const auto tokens = paletteTokens(c);
    apply(content, tokens);
    // Qt returns children before descendants: inherited palettes are ready first.
    for (auto *widget : content->findChildren<QWidget *>())
        if (widget->property("vlt.type").isValid())
            apply(widget, tokens);
    content->setAutoFillBackground(true);
}
} // namespace ui::scriptstyle
