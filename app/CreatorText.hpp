#pragma once
#include <QCoreApplication>
#include <QString>
#include <string>
#include <string_view>
namespace ui {
inline QString creatorText(std::string_view value) {
  const std::string key(value);
  return QCoreApplication::translate("CreatorNodes", key.c_str());
}
} // namespace ui
