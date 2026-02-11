Import("env")

import os
import re
import shutil


def _decode_define_value(value):
  if not isinstance(value, str):
    return ""

  decoded = value.replace('\\"', '"').strip()
  if decoded.startswith('"') and decoded.endswith('"') and len(decoded) >= 2:
    decoded = decoded[1:-1]
  return decoded


def _get_crosspoint_version():
  for define in env.get("CPPDEFINES", []):
    if isinstance(define, (tuple, list)) and len(define) >= 2 and define[0] == "CROSSPOINT_VERSION":
      return _decode_define_value(define[1])
  return "unknown"


def _sanitize_filename_part(value):
  sanitized = re.sub(r"[^A-Za-z0-9._-]+", "_", value).strip("._-")
  return sanitized or "unknown"


def _get_unique_output_path(project_dir, base_file_name):
  root_name, extension = os.path.splitext(base_file_name)
  candidate = os.path.join(project_dir, base_file_name)
  if not os.path.exists(candidate):
    return candidate

  suffix = 1
  while True:
    candidate = os.path.join(project_dir, "{}_{}{}".format(root_name, suffix, extension))
    if not os.path.exists(candidate):
      return candidate
    suffix += 1


def _export_versioned_firmware(target, source, env):
  project_dir = env.subst("$PROJECT_DIR")
  source_firmware = env.subst("$BUILD_DIR/${PROGNAME}.bin")

  if not os.path.exists(source_firmware):
    print("[bin] Source firmware not found at {}".format(source_firmware))
    return

  version = _sanitize_filename_part(_get_crosspoint_version())
  output_name = "{}-firmware.bin".format(version)
  output_path = _get_unique_output_path(project_dir, output_name)

  shutil.copy2(source_firmware, output_path)
  print("[bin] Copied firmware to {}".format(output_path))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _export_versioned_firmware)
