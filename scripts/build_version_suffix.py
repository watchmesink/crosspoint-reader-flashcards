Import("env")

import os


def _encode_define_value(value):
  return '\\"{}\\"'.format(value)


VERSION_BASE = "0.1"
COUNTER_FILE_NAME = ".gleb_version_counter"


def _read_counter(path):
  try:
    with open(path, "r", encoding="utf-8") as counter_file:
      raw = counter_file.read().strip()
      if raw == "":
        return 0
      return max(0, int(raw))
  except Exception:
    return 0


def _write_counter(path, value):
  with open(path, "w", encoding="utf-8") as counter_file:
    counter_file.write(str(value))


def _next_patch_number():
  project_dir = env.subst("$PROJECT_DIR")
  counter_path = os.path.join(project_dir, COUNTER_FILE_NAME)
  current = _read_counter(counter_path)
  next_value = current + 1
  _write_counter(counter_path, next_value)
  return next_value


cpp_defines = []
for define in env.get("CPPDEFINES", []):
  if isinstance(define, (tuple, list)) and len(define) >= 1 and define[0] == "CROSSPOINT_VERSION":
    continue
  if isinstance(define, str) and define.startswith("CROSSPOINT_VERSION"):
    continue
  cpp_defines.append(define)

patch_number = _next_patch_number()
version_with_suffix = "{}.{}-gleb".format(VERSION_BASE, patch_number)

cpp_defines.append(("CROSSPOINT_VERSION", _encode_define_value(version_with_suffix)))
env.Replace(CPPDEFINES=cpp_defines)
print("[ver] CROSSPOINT_VERSION={}".format(version_with_suffix))
