"""
test/src/*.pn を全てコンパイルして test/asm/ へ出力するテストスクリプト。
コンパイラをビルドしてから各 .pn を .pt に変換し、期待値と一致するか確認する。
asm_ans/ に期待値ファイルがなければ FAIL とする。
"""

import difflib
import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
COMPILER_DIR = os.path.dirname(SCRIPT_DIR)
SRC_DIR = os.path.join(SCRIPT_DIR, "src")
ASM_DIR = os.path.join(SCRIPT_DIR, "asm")
ASM_ANS_DIR = os.path.join(SCRIPT_DIR, "asm_ans")
C2ASM = os.path.join(COMPILER_DIR, "c2asm.exe")

# ビルド対象のソース一式
SOURCES = ["lexer.cpp", "parser.cpp", "analyzer.cpp", "generator.cpp", "c2asm.cpp"]

def build_compiler():
    """コンパイラをビルドする。成功すれば True を返す。"""
    cmd = ["g++", "-Wall", "-Wextra", "-std=c++17", "-o", C2ASM]
    cmd += [os.path.join(COMPILER_DIR, s) for s in SOURCES]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print("[BUILD FAIL] コンパイラのビルドに失敗しました")
        print(result.stderr.strip())
        return False
    return True

def main():
    # まずコンパイラをビルドする
    if not build_compiler():
        sys.exit(1)

    os.makedirs(ASM_DIR, exist_ok=True)

    src_files = sorted(
        f for f in os.listdir(SRC_DIR) if f.endswith(".pn")
    )

    if not src_files:
        print("テストケースが見つかりません。")
        sys.exit(1)

    compile_success = []
    compile_fail = []
    compare_pass = []
    compare_fail = []

    for src_file in src_files:
        src_path = os.path.join(SRC_DIR, src_file)
        asm_name = src_file.replace(".pn", ".pt")
        asm_path = os.path.join(ASM_DIR, asm_name)
        ans_path = os.path.join(ASM_ANS_DIR, asm_name)

        result = subprocess.run(
            [C2ASM, "-pn", src_path, "-pt", asm_path],
            capture_output=True,
            text=True,
        )

        stdout = result.stdout.strip()
        stderr = result.stderr.strip()
        output = (stdout + "\n" + stderr).strip()

        if result.returncode != 0:
            compile_fail.append((src_file, output))
            print(f"[FAIL] {src_file}: {output}")
            continue

        compile_success.append(src_file)

        # asm_ans/ に期待値ファイルがなければエラー
        if not os.path.exists(ans_path):
            compare_fail.append(src_file)
            print(f"[FAIL] {src_file}  (asm_ans/{asm_name} が存在しません)")
            continue

        with open(asm_path, encoding="utf-8") as f:
            actual_lines = f.readlines()
        with open(ans_path, encoding="utf-8") as f:
            expected_lines = f.readlines()

        if actual_lines == expected_lines:
            compare_pass.append(src_file)
            print(f"[PASS] {src_file}")
        else:
            compare_fail.append(src_file)
            diff = difflib.unified_diff(
                expected_lines,
                actual_lines,
                fromfile=f"expected (asm_ans/{asm_name})",
                tofile=f"actual   (asm/{asm_name})",
            )
            print(f"[FAIL] {src_file}")
            for line in "".join(diff).splitlines():
                print(f"  {line}")

    print()
    print(f"コンパイル成功: {len(compile_success)}件 / 失敗: {len(compile_fail)}件")
    print(f"期待値比較  PASS: {len(compare_pass)}件 / FAIL: {len(compare_fail)}件")

    if compile_fail or compare_fail:
        sys.exit(1)

if __name__ == "__main__":
    main()
