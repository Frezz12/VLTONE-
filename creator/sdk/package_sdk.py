"""Assemble the self-contained SDK reference and a shareable documentation kit."""
import argparse
import json
from pathlib import Path
import re
import zipfile

ROOT = Path(__file__).resolve().parent
MARKER = "<!-- SDK_REFERENCE_APPENDICES -->"
SOURCES = ["Gain", "LocalHelpers", "OnePole", "MultiOutput", "Delay", "Chorus",
           "ControlLFO", "SeededNoise", "Saturate", "CallSaturate", "TwoFilters",
           "NestedCall", "CallMultiOutput"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zip", required=True, type=Path)
    args = parser.parse_args()
    guide = ROOT / "CREATOR_SDK_RU.md"
    original = guide.read_text(encoding="utf-8")
    if MARKER not in original:
        raise RuntimeError("Reference appendix marker is missing")
    header = (ROOT / "vlt/creator.hpp").read_text(encoding="utf-8").rstrip()
    appendix = [
        "## Приложение A Полный заголовок SDK\n",
        "Это точная текстовая копия `vlt/creator.hpp` из данного комплекта. "
        "При создании ноды подключай заголовок через `#include <vlt/creator.hpp>`; "
        "саму реализацию SDK в исходник ноды переносить не требуется.\n",
        f"```cpp\n{header}\n```\n",
        "## Приложение B Полные исходники примеров\n",
        "Каждый следующий блок — полный исходник одной C++-ноды. "
        "Для самостоятельных эффектов Entry function равна `process`; "
        "в Saturate — `saturate`. Ноды с Function-входами требуют "
        "янтарных связей, описанных в руководстве и сохранённых в готовых проектах.\n",
    ]
    for name in SOURCES:
        source = (ROOT / "examples" / f"{name}.cpp").read_text(encoding="utf-8").rstrip()
        appendix += [f"### Пример {name}\n", f"Файл: `examples/{name}.cpp`.\n",
                     f"```cpp\n{source}\n```\n"]
    text = original.split(MARKER, 1)[0].rstrip() + "\n\n" + MARKER + "\n\n" + "\n".join(appendix)
    headings = [name for name in re.findall(r"^## (.+)$", text, re.MULTILINE)
                if name != "Содержание"]
    def anchor(name):
        return re.sub(r"\s", "-", re.sub(r"[^\w\s-]", "", name.lower()))
    contents = "## Содержание\n\n" + "\n".join(
        f"- [{name}](#{anchor(name)})" for name in headings)
    text = re.sub(r"<!-- SDK_TOC_START -->.*?<!-- SDK_TOC_END -->",
                  "<!-- SDK_TOC_START -->\n\n" + contents +
                  "\n\n<!-- SDK_TOC_END -->", text, flags=re.DOTALL)
    guide.write_text(text, encoding="utf-8")

    report = json.loads((ROOT / "examples/projects/verification.json").read_text(encoding="utf-8"))
    if len(report["examples"]) != 12 or not report.get("extraction"):
        raise RuntimeError("Examples have not completed verification")
    for item in report["examples"]:
        if not item.get("analysis") or not item.get("wasm"):
            raise RuntimeError(f"Incomplete example: {item['name']}")
        for suffix in (".vltcreator", ".vltmini"):
            path = ROOT / "examples/projects" / (item["name"] + suffix)
            data = json.loads(path.read_text(encoding="utf-8"))
            if not data["definition"]["code"].get("wasm"):
                raise RuntimeError(f"Missing portable code: {path}")

    for document in ROOT.glob("*.md"):
        content = document.read_text(encoding="utf-8")
        if len(re.findall(r"^```", content, re.MULTILINE)) % 2:
            raise RuntimeError(f"Unbalanced code fences: {document.name}")
        prose = re.sub(r"^```[^\n]*\n.*?^```[ \t]*$", "", content,
                       flags=re.MULTILINE | re.DOTALL)
        prose = re.sub(r"`[^`]*`", "", prose)
        for target in re.findall(r"\[[^\]\n]+\]\(([^)\n]+)\)", prose):
            if "://" not in target and not target.startswith("#"):
                destination = target.split("#", 1)[0]
                if destination and not (document.parent / destination).exists():
                    raise RuntimeError(f"Broken link in {document.name}: {target}")

    destination = args.zip.resolve()
    if destination.is_relative_to(ROOT):
        raise RuntimeError("Place the archive outside its SDK source folder")
    destination.parent.mkdir(parents=True, exist_ok=True)
    files = [path for path in ROOT.rglob("*") if path.is_file()
             and "__pycache__" not in path.parts and path.suffix != ".pyc"]
    with zipfile.ZipFile(destination, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(files):
            archive.write(path, Path("Creator-SDK-1") / path.relative_to(ROOT))
    with zipfile.ZipFile(destination) as archive:
        if archive.testzip() is not None:
            raise RuntimeError("Archive CRC verification failed")
    print(json.dumps({"files": len(files), "source_files": len(SOURCES),
                      "projects": len(report["examples"]),
                      "guide_characters": len(text), "zip_bytes": destination.stat().st_size,
                      "archive": str(destination)}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
