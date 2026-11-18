#!/usr/bin/env python3
"""把一个 UTF-8 JSON 文件嵌进 C 头文件（编译期嵌入）。

用法：
    python3 tools/gen_embed.py <input.json> <output.h> <GUARD> <VAR>

例如（cpp 仓库，规则来自 C 版仓库的 rules.json）：
    python3 tools/gen_embed.py /home/hatch/workspace/cdc-c/rules.json \\
        src/rules_data.h CDC_RULES_DATA_H CDC_RULES_JSON

要点（2026-11-18 乱码 bug 的教训，勿重蹈）：
- 输入必须按 UTF-8 解码（严格模式，非法字节直接报错，绝不静默降级）。
  旧脚本（/tmp/gen_embed.py，已丢失）把 UTF-8 当 Latin-1 读再存回 UTF-8，
  每个汉字都被双重编码，编出来的 exe 里所有规则名/描述全是乱码
  （rules_data.h 里正确的“可”0 次、双重编码的 323 次）。
- 输出的 C 字符串字面量里，非 ASCII 字节原样保留（UTF-8）；配合
  MSVC /utf-8（顶层 CMakeLists 已加）编译后即为正确的 UTF-8。
- 转义只做三件事：\\ -> \\\\、" -> \\"、换行 -> \\n；不碰其他字节。
  若输入含换行/制表/回车以外的控制字符，直接报错（不要悄悄改写）。
- 折行按 200 列，且不断开转义序列（行尾不留单个 \\）。
"""

import sys


def escape_json_text(text: str) -> str:
    out = []
    for ch in text:
        o = ord(ch)
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif o < 0x20:
            raise ValueError(f"unexpected control character U+{o:04X} in input")
        else:
            out.append(ch)
    return "".join(out)


def wrap(escaped: str, width: int = 200) -> list:
    lines = []
    i = 0
    n = len(escaped)
    while i < n:
        j = min(i + width, n)
        # 不断开转义序列：所有转义都是 2 字符且以 \ 开头；若行尾连续 \ 为奇数个，
        # 最后一个 \ 会转义行尾引号，须回退一位（回退后连续 \ 变偶数，即完整）。
        while j > i:
            k = j
            while k > i and escaped[k - 1] == "\\":
                k -= 1
            if (j - k) % 2 == 1:
                j -= 1
            else:
                break
        lines.append(escaped[i:j])
        i = j
    return lines


def main(argv) -> int:
    if len(argv) != 5:
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    src_path, dst_path, guard, var = argv[1:5]
    raw = open(src_path, "rb").read()
    text = raw.decode("utf-8")  # 严格 UTF-8：非法字节直接抛错
    escaped = escape_json_text(text)
    body = wrap(escaped)
    with open(dst_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("/* 由工具生成，勿手改。\n")
        f.write(f" * 来源：{src_path}（与 Rust 版 include_str! 行为一致：编译期嵌入，\n")
        f.write(" * 运行时不读文件，防篡改后放宽 allowlist / 规则）。\n")
        f.write(f" * 再生成：python3 tools/gen_embed.py {src_path} {dst_path} {guard} {var}\n")
        f.write(" */\n")
        f.write(f"#ifndef {guard}\n")
        f.write(f"#define {guard}\n")
        f.write("\n")
        f.write(f"/* {len(raw)} 字节。*/\n")
        f.write(f"static const char {var}[] =\n")
        for line in body:
            f.write(f'"{line}"\n')
        f.write(";\n")
        f.write("\n")
        f.write(f"#endif /* {guard} */\n")
    print(f"{src_path} ({len(raw)} bytes, UTF-8 OK) -> {dst_path} ({len(body)} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
