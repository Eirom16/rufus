#!/usr/bin/env python3
"""
Translate all .ts files using googletrans (free, no API key needed).
Sequential translation with delays to avoid rate limiting.
Preserves Qt format specifiers (%1, %2, %n, \n).

Usage: python3 tools/translate-ts.py
"""

import asyncio
import os
import re
import xml.etree.ElementTree as ET

from googletrans import Translator

LANGUAGES = {
    "es": "es",
    "pt_BR": "pt",
    "ru": "ru",
    "fr": "fr",
    "de": "de",
}

TS_DIR = os.path.join(os.path.dirname(__file__), "..", "src", "translations")

QT_PLACEHOLDER_RE = re.compile(r"(%[1-9n]|\\n)")

def protect_placeholders(text):
    parts = QT_PLACEHOLDER_RE.split(text)
    mapping = {}
    protected = []
    idx = 0
    for part in parts:
        if QT_PLACEHOLDER_RE.match(part):
            key = f"\x00P{idx}\x00"
            mapping[key] = part
            protected.append(key)
            idx += 1
        else:
            protected.append(part)
    return "".join(protected), mapping

def restore_placeholders(text, mapping):
    for key, value in mapping.items():
        text = text.replace(key, value)
    return text

async def translate_file(suffix, lang_code):
    ts_path = os.path.join(TS_DIR, f"rufus-qt_{suffix}.ts")
    if not os.path.exists(ts_path):
        print(f"  SKIP: {ts_path} not found")
        return

    print(f"\n=== {suffix} ({lang_code}) ===")

    tree = ET.parse(ts_path)
    root = tree.getroot()

    messages = []
    for msg in root.iter("message"):
        source = msg.find("source")
        trans = msg.find("translation")
        if source is not None and trans is not None:
            text = source.text or ""
            if text.strip():
                messages.append((text, trans))

    total = len(messages)
    print(f"  {total} strings")

    translator = Translator()
    translated = 0

    for idx, (text, trans_elem) in enumerate(messages):
        if trans_elem.text and trans_elem.text.strip():
            continue

        protected, mapping = protect_placeholders(text)
        if not protected.strip():
            continue

        try:
            result = await translator.translate(protected, dest=lang_code, src="en")
            translated_text = restore_placeholders(result.text, mapping)
            trans_elem.text = translated_text
            if "type" in trans_elem.attrib:
                del trans_elem.attrib["type"]
            translated += 1
        except Exception as e:
            print(f"  [{idx+1}/{total}] Error: {e}")
            continue

        if (idx + 1) % 10 == 0:
            print(f"  [{idx+1}/{total}] {translated} done")

        await asyncio.sleep(0.3)

    tree.write(ts_path, xml_declaration=True, encoding="utf-8")
    print(f"  Done: {translated}/{total} translated")

async def main():
    for suffix, lang_code in LANGUAGES.items():
        await translate_file(suffix, lang_code)
    print("\nAll done!")

if __name__ == "__main__":
    asyncio.run(main())
