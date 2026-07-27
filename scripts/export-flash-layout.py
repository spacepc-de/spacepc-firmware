Import("env")

import json
from pathlib import Path


def export_flash_layout(source, target, env):
    parts = []
    for offset, image in env.get("FLASH_EXTRA_IMAGES", []):
        image_path = Path(env.subst(str(image))).resolve()
        if image_path.exists():
            parts.append({
                "offset": int(env.subst(str(offset)), 16),
                "source": str(image_path),
            })

    application_path = Path(env.subst("$BUILD_DIR/${PROGNAME}.bin")).resolve()
    parts.append({
        "offset": int(env.subst("$ESP32_APP_OFFSET"), 16),
        "source": str(application_path),
    })

    output = Path(env.subst("$BUILD_DIR/flash-layout.json"))
    output.write_text(json.dumps({"parts": parts}, indent=2) + "\n")
    print(f"Exported verified flash layout to {output}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", export_flash_layout)
