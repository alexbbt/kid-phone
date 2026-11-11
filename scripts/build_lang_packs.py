#!/usr/bin/env python3
"""Build lang-packs/<lang>/manifest.json (+ optional file copies) for OTA phones.

English stays in firmware. Each other language gets a manifest that points at
raw GitHub (or any HTTPS) URLs for the PCM/WAV clips already in the repo.

Usage:
  scripts/build_lang_packs.py
  scripts/build_lang_packs.py --base https://raw.githubusercontent.com/org/repo/branch

Writes lang-packs/<lang>/manifest.json for ca, es, fr, it, nl, de, vi, pt-br.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "lang-packs"

CLIPS = [
    "time_up",
    "unavailable",
    "no_answer",
    "no_wifi",
    "not_ready",
    "cant_call",
    "sleep_time",
    "secret_start",
    "secret_sent",
]

# prompt_lang code → filename suffix (_ca, _pt_br, …)
LANGS = {
    "ca": "ca",
    "es": "es",
    "fr": "fr",
    "it": "it",
    "nl": "nl",
    "de": "de",
    "vi": "vi",
    "pt-br": "pt_br",
}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--base",
        default="https://raw.githubusercontent.com/alexbbt/kid-phone/working/integrations",
        help="Repo raw root (no trailing slash); manifests use {base}/src/prompts/…",
    )
    args = ap.parse_args()
    base = args.base.rstrip("/")

    for code, sfx in LANGS.items():
        files = []
        for clip in CLIPS:
            name = f"{clip}.pcm"
            src = f"{clip}_{sfx}.pcm"
            path = ROOT / "src" / "prompts" / src
            if not path.exists():
                raise SystemExit(f"missing {path}")
            files.append(
                {
                    "name": name,
                    "url": f"{base}/src/prompts/{src}",
                    "size": path.stat().st_size,
                }
            )
        wav = ROOT / "twilio" / f"no_answer_{sfx}.wav"
        if not wav.exists():
            raise SystemExit(f"missing {wav}")
        files.append(
            {
                "name": "no_answer.wav",
                "url": f"{base}/twilio/no_answer_{sfx}.wav",
                "size": wav.stat().st_size,
            }
        )
        dest = OUT / code
        dest.mkdir(parents=True, exist_ok=True)
        manifest = {"lang": code, "version": 1, "files": files}
        (dest / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        print(f"wrote {dest / 'manifest.json'} ({len(files)} files)")


if __name__ == "__main__":
    main()
