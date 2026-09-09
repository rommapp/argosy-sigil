# SPDX-License-Identifier: MPL-2.0
"""Cross-language contract tests.

The C `sigil_usage` enum, the Python and CLI string maps, the Kotlin `Usage`
enum, and the JNI constructor descriptor are five hand-maintained copies of
the same facts. Nothing at compile time keeps them in step: a JNI descriptor
that disagrees with the Kotlin constructor only fails at first extraction on a
device, and a usage value missing from a string map only shows up at runtime.

These tests parse the sources directly (no compiled extension, no Android
toolchain) so a drift between any two copies fails in plain CI.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "include" / "sigil.h"
CLI = ROOT / "cli" / "sigil.c"
PY_INIT = ROOT / "bindings" / "python" / "sigil" / "__init__.py"
KOTLIN = ROOT / "bindings" / "android" / "src" / "main" / "kotlin" / "com" / "nendo" / "sigil" / "Sigil.kt"
JNI = ROOT / "bindings" / "android" / "src" / "main" / "cpp" / "sigil_jni.c"

# Kotlin/JVM primitive and reference types to their field descriptors.
_JVM_DESCRIPTORS = {
    "String": "Ljava/lang/String;",
    "List": "Ljava/util/List;",
    "Int": "I",
    "Long": "J",
    "Boolean": "Z",
    "Float": "F",
    "Double": "D",
}

# Kotlin classes the JNI constructs, keyed by the global jclass that holds them.
_JNI_CLASSES = {
    "SigilResult": "g_result_class",
    "SigilSaveMember": "g_member_class",
    "SigilSaveUnit": "g_unit_class",
    "SigilException": "g_exception_class",
}

# One row per operation the C API offers, with the name each binding gives it.
# A binding that lacks a row is not a binding of that operation.
_API_SURFACE = {
    "version": ("fun version(", "def version(", "func Version("),
    "platform slug": ("fun platformSlug(", "def platform_from_slug(", "func PlatformFromSlug("),
    "extract": ("fun extract(", "def extract(", "func Extract("),
    "header key from prod.keys": (
        "fun loadHeaderKeyFromProdKeys(",
        "def load_header_key_from_prod_keys(",
        "func LoadHeaderKeyFromProdKeys(",
    ),
    "persisted result": ("fun persisted(", "def persisted(", "func PersistedResult("),
    "locate saves": ("fun locateSaves(", "def locate_saves(", "func LocateSaves("),
    "hash saves": ("fun hashSaves(", "def hash_saves(", "func HashSaves("),
    "layout subdirs": ("fun layoutSubdirs(", "def layout_subdirs(", "func LayoutSubdirs("),
    "content stem": ("fun contentStem(", "def content_stem(", "func ContentStem("),
    "list save root": ("fun listSaveRoot(", "def list_save_root(", "func ListSaveRoot("),
}

_EXTRACT_OPTIONS = {
    "filename fallback": ("filenameFallback", "filename_fallback", "DisableFilenameFallback"),
    "3ds homebrew": ("allow3dsHomebrew", "allow_3ds_homebrew", "Allow3DSHomebrew"),
    "switch header key": ("headerKey", "header_key", "SwitchHeaderKey"),
    "switch prod.keys path": ("prodKeysPath", "prod_keys_path", "SwitchProdKeysPath"),
    "switch prod.keys text": ("prodKeysText", "prod_keys_text", "SwitchProdKeysBlob"),
}

_RESULT_FIELDS = {
    "title id": ("titleId", "title_id", "TitleID"),
    "raw serial": ("rawSerial", "raw_serial", "RawSerial"),
    "save id": ("saveId", "save_id", "SaveID"),
    "platform slug": ("platformSlug", "platform", "PlatformSlug"),
    "source": ("source", "source", "Source"),
    "usage": ("usage", "usage", "Usage"),
    "experimental": ("experimental", "experimental", "Experimental"),
    "switch content type": ("switchContentType", "switch_content_type", "SwitchContentType"),
    "title version": ("titleVersion", "title_version", "TitleVersion"),
    "features": ("features", "features", "Features"),
}

_UNIT_FIELDS = {
    "key": ("key", "key", "Key"),
    "shape": ("shape", "shape", "Shape"),
    "members": ("members", "members", "Members"),
    "expected": ("expected", "expected", "Expected"),
    "unkeyed": ("unkeyed", "unkeyed", "Unkeyed"),
    "artifact": ("artifact", "artifact", "Artifact"),
    "content hash": ("contentHash", "content_hash", "ContentHash"),
    "identity hash": ("identityHash", "identity_hash", "IdentityHash"),
}

GO = ROOT / "bindings" / "go" / "sigil.go"
KEEP_RULES = ROOT / "bindings" / "android" / "consumer-rules.pro"


def _canonical_string(enum_name: str) -> str:
    """FOLDER_EXACT -> folder-exact: the string form the maps must emit."""
    return enum_name.lower().replace("_", "-")


def _pascal_to_upper_snake(name: str) -> str:
    """FolderSplit -> FOLDER_SPLIT, to compare Kotlin names against C names."""
    return re.sub(r"(?<!^)(?=[A-Z])", "_", name).upper()


def _c_usage_enum() -> list[tuple[str, int]]:
    block = re.search(
        r"typedef enum\s*\{(.*?)\}\s*sigil_usage;", HEADER.read_text(), re.DOTALL
    )
    assert block, "sigil_usage enum not found in sigil.h"
    pairs = re.findall(r"SIGIL_USAGE_(\w+)\s*=\s*(\d+)", block.group(1))
    assert pairs, "no SIGIL_USAGE_* members parsed"
    return [(name, int(value)) for name, value in pairs]


def _kotlin_usage_entries() -> list[tuple[str, int]]:
    # Enum entries run from the opening brace to the ';' before the companion.
    block = re.search(
        r"enum class Usage\(val code: Int\)\s*\{(.*?);", KOTLIN.read_text(), re.DOTALL
    )
    assert block, "Kotlin Usage enum not found"
    entries = re.findall(r"(\w+)\((\d+)\)", block.group(1))
    return [(_pascal_to_upper_snake(n), int(v)) for n, v in entries]


def _kotlin_ctor_types(class_name: str) -> list[str]:
    block = re.search(
        rf"\bclass {class_name}\((.*?)\)", KOTLIN.read_text(), re.DOTALL
    )
    assert block, f"Kotlin {class_name} constructor not found"
    types = re.findall(r"(?:^|,)\s*(?:private\s+)?(?:val\s+)?\w+:\s*(\w+)", block.group(1))
    assert types, f"no {class_name} constructor params parsed"
    return types


def test_python_usage_map_covers_c_enum():
    c_names = {name for name, _ in _c_usage_enum()}
    py_map = dict(
        re.findall(r'lib\.SIGIL_USAGE_(\w+):\s*"([^"]+)"', PY_INIT.read_text())
    )
    assert set(py_map) == c_names, (
        "_USAGE_NAMES is out of sync with sigil_usage: "
        f"missing {c_names - set(py_map)}, extra {set(py_map) - c_names}"
    )
    for name, string in py_map.items():
        assert string == _canonical_string(name), f"{name} maps to {string!r}"


def test_cli_usage_switch_covers_c_enum():
    c_names = {name for name, _ in _c_usage_enum()}
    cli_map = dict(
        re.findall(r'case SIGIL_USAGE_(\w+):\s*return\s*"([^"]+)";', CLI.read_text())
    )
    assert c_names <= set(cli_map), (
        f"usage_to_str is missing cases: {c_names - set(cli_map)}"
    )
    for name, string in cli_map.items():
        assert string == _canonical_string(name), f"{name} returns {string!r}"


def test_kotlin_usage_enum_matches_c_enum():
    kotlin = _kotlin_usage_entries()
    c_enum = _c_usage_enum()
    assert kotlin == c_enum, (
        f"Kotlin Usage enum {kotlin} disagrees with sigil_usage {c_enum}"
    )


def _kotlin_ctor_descriptor(class_name: str) -> str:
    tokens = []
    for t in _kotlin_ctor_types(class_name):
        assert t in _JVM_DESCRIPTORS, f"unmapped Kotlin type {t!r}"
        tokens.append(_JVM_DESCRIPTORS[t])
    return "(" + "".join(tokens) + ")V"


def _jni_ctor_descriptor(global_class: str) -> str:
    match = re.search(
        rf'(?:GetMethodID|find_method)\(env,\s*{global_class},\s*"<init>",\s*"([^"]+)"',
        JNI.read_text(),
        re.DOTALL,
    )
    assert match, f"<init> lookup for {global_class} not found in sigil_jni.c"
    return match.group(1)


def _jni_ctor_args(global_class: str) -> list[str]:
    match = re.search(
        rf"NewObject\(env,\s*{global_class},\s*\w+,\s*(.*?)\);", JNI.read_text(), re.DOTALL
    )
    assert match, f"NewObject for {global_class} not found in sigil_jni.c"
    return [a.strip() for a in match.group(1).split(",")]


def test_jni_descriptors_match_kotlin_constructors():
    """The bug in cde2301: the JNI descriptor kept a String param the Kotlin
    constructor had dropped, so GetMethodID missed the constructor and every
    extraction crashed. This locks each pair together."""
    for class_name, global_class in _JNI_CLASSES.items():
        assert _jni_ctor_descriptor(global_class) == _kotlin_ctor_descriptor(class_name), (
            f"{class_name} descriptor drifted"
        )


def test_jni_string_args_match_descriptors():
    jstring_locals = set(re.findall(r"\bjstring\s+(\w+)\s*=", JNI.read_text()))
    for class_name, global_class in _JNI_CLASSES.items():
        string_params = _jni_ctor_descriptor(global_class).count("Ljava/lang/String;")
        string_args = [a for a in _jni_ctor_args(global_class) if a in jstring_locals]
        assert len(string_args) == string_params, (
            f"{class_name}: {len(string_args)} jstring args feed a "
            f"{string_params}-String constructor"
        )


def _jni_exception_thrown_on_failure() -> bool:
    return "throw_sigil(env, rc)" in JNI.read_text()


def _binding_sources() -> dict[str, str]:
    return {"kotlin": KOTLIN.read_text(), "python": PY_INIT.read_text(), "go": GO.read_text()}


def _assert_each_binding_has(table: dict[str, tuple[str, str, str]], what: str) -> None:
    sources = _binding_sources()
    for operation, names in table.items():
        for language, name in zip(("kotlin", "python", "go"), names):
            assert name in sources[language], f"{language} binding lacks {what} {operation!r} ({name})"


def test_every_binding_exposes_every_operation():
    _assert_each_binding_has(_API_SURFACE, "operation")


def test_every_binding_takes_every_extract_option():
    _assert_each_binding_has(_EXTRACT_OPTIONS, "extract option")


def test_every_binding_returns_every_result_field():
    _assert_each_binding_has(_RESULT_FIELDS, "result field")


def test_every_binding_returns_every_save_unit_field():
    _assert_each_binding_has(_UNIT_FIELDS, "save unit field")


def _keep_rule_patterns() -> list[re.Pattern[str]]:
    patterns = []
    for name in re.findall(r"^-keep\s+class\s+([\w.$*]+)", KEEP_RULES.read_text(), re.MULTILINE):
        regex = re.escape(name).replace(r"\*\*", r"[\w.$]+").replace(r"\*", r"[\w$]+")
        patterns.append(re.compile(rf"^{regex}$"))
    return patterns


def _jni_looked_up_classes() -> set[str]:
    names = re.findall(r'(?:FindClass|global_class)\(env,\s*"(com/nendo/sigil/[\w$/]+)"', JNI.read_text())
    return {n.replace("/", ".") for n in names}


def _kotlin_binding_classes() -> set[str]:
    text = KOTLIN.read_text()
    names = set(re.findall(r"^(?:data |enum )?class (\w+)", text, re.MULTILINE))
    names |= set(re.findall(r"^object (\w+)", text, re.MULTILINE))
    return {f"com.nendo.sigil.{n}" for n in names}


def test_keep_rules_cover_every_class_the_jni_looks_up():
    """v2.15.0 shipped without a keep rule for SigilSaveMember; R8 renamed it,
    FindClass failed, and the app crash-looped on every start (#429)."""
    patterns = _keep_rule_patterns()
    assert patterns, "no -keep class rules in consumer-rules.pro"
    for name in sorted(_jni_looked_up_classes() | _kotlin_binding_classes()):
        assert any(p.match(name) for p in patterns), f"consumer-rules.pro does not keep {name}"


def test_jni_never_stacks_a_lookup_on_a_pending_exception():
    """The second FindClass after a failed one is what turns a missing class into
    a SIGABRT; every lookup goes through the clearing helpers."""
    text = JNI.read_text()
    raw = re.findall(r"\(\*env\)->FindClass\(env,\s*\"([^\"]+)\"", text)
    assert all(n in ("java/lang/IllegalStateException", "java/lang/String") for n in raw), (
        f"direct FindClass on {raw}; use global_class/find_class"
    )
    assert "ExceptionClear" in text


def test_every_binding_reports_the_c_error_code():
    sources = _binding_sources()
    assert "class SigilException(val code: Int" in sources["kotlin"]
    assert _jni_exception_thrown_on_failure()
    assert "self.code = code" in sources["python"]
    assert "func errFromCode(rc C.int) error" in sources["go"]
