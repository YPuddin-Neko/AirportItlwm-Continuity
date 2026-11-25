"""Use the driver's wire declarations in transport tests."""

import re
from test_regressions import ROOT


def receive_diagnostics():
    header = (ROOT / "itlwm/hal_iwx/ItlIwx.hpp").read_text()
    return "\n".join(re.search(r"struct " + name + r" \{.*?\n\};", header, re.S)[0]
        for name in ("IwxAWDLReceiveSample", "IwxAWDLReceiveDiagnostics"))


def declarations(structs, text="", enums=()):
    headers = "\n".join((ROOT / "itlwm/hal_iwx" / name).read_text()
                        for name in ("if_iwxreg.h", "if_iwxvar.h"))
    parts = ["#define __packed __attribute__((packed))", "#define ETHER_ADDR_LEN 6"]
    defined = set()
    for name in enums:
        declaration = re.search(r"enum " + name + r"\s*\{.*?\};", headers, re.S)[0]
        parts.append(declaration)
        defined.update(re.findall(r"\bIWX_\w+", declaration))
    for name in structs:
        start = re.search(r"struct " + name + r"\s*\{", headers).start()
        end = headers.index("{", start) + 1
        depth = 1
        while depth:
            depth += (headers[end] == "{") - (headers[end] == "}")
            end += 1
        parts.append(headers[start:headers.index(";", end) + 1])
    input_text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text + "\n".join(parts), flags=re.S)
    defined.update(re.findall(r"^#define\s+(IWX_\w+)", input_text, re.M))
    pending = set(re.findall(r"\bIWX_\w+", input_text)) - defined
    macros = {}
    while pending:
        name = pending.pop()
        if name in macros:
            continue
        match = re.search(r"^#define[ \t]+" + name + r"\b(?:[^\n\\]|\\\n)*", headers, re.M)
        if not match:
            raise ValueError("Missing firmware definition: " + name)
        macros[name] = match[0]
        pending.update(set(re.findall(r"\bIWX_\w+", match[0])) - defined - macros.keys())
    return "\n".join(macros[name] for name in sorted(macros)) + "\n" + "\n".join(parts)
