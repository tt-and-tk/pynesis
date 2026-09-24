"""
test/src/*.pn・test/src_bin/*.pn を全てコンパイルして test/asm/・test/asm_bin/ へ出力するテストスクリプト。
コンパイラをビルドしてから各 .pn を .pt に変換し、期待値と一致するか確認する。
src/ はROM用、src_bin/ はQosmosの実行ファイル用(--bin-mode)としてコンパイルする。
期待値ファイル(asm_ans/・asm_bin_ans/)がなければ FAIL とする。
"""

import difflib
import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
COMPILER_DIR = os.path.dirname(SCRIPT_DIR)
PN2ASM = os.path.join(COMPILER_DIR, "pn2asm.exe")

# テストの組: (入力ディレクトリ, 出力ディレクトリ, 期待値ディレクトリ, コンパイラへ追加で渡す引数)
SUITES = [
    ("src", "asm", "asm_ans", []),
    ("src_bin", "asm_bin", "asm_bin_ans", ["--bin-mode"]),
]

# ビルド対象のソース一式
SOURCES = ["lexer.cpp", "parser.cpp", "analyzer.cpp", "generator.cpp", "pn2asm.cpp"]

def build_compiler():
    """コンパイラをビルドする。成功すれば True を返す。"""
    cmd = ["g++", "-Wall", "-Wextra", "-std=c++17", "-o", PN2ASM]
    cmd += [os.path.join(COMPILER_DIR, s) for s in SOURCES]
    # g++の警告はソース中の日本語コメントを引用するため，Windows既定のcp932ではなくUTF-8で読む
    result = subprocess.run(cmd, capture_output=True, encoding="utf-8", errors="backslashreplace")
    if result.returncode != 0:
        print("[BUILD FAIL] コンパイラのビルドに失敗しました")
        print(result.stderr.strip())
        return False
    return True

def run_suite(src_name, asm_name_dir, ans_name_dir, extra_args,
              compile_success, compile_fail, compare_pass, compare_fail):
    """1組のテストを実行し，結果を各リストへ追加する。テストケースが1件もなければ False を返す。"""
    src_dir = os.path.join(SCRIPT_DIR, src_name)
    asm_dir = os.path.join(SCRIPT_DIR, asm_name_dir)
    ans_dir = os.path.join(SCRIPT_DIR, ans_name_dir)

    os.makedirs(asm_dir, exist_ok=True)

    src_files = sorted(
        f for f in os.listdir(src_dir) if f.endswith(".pn")
    )

    if not src_files:
        print(f"テストケースが見つかりません。({src_name}/)")
        return False

    for src_file in src_files:
        # 結果の表示では，どの組のテストかが分かるようディレクトリ名を付ける
        case_name = f"{src_name}/{src_file}"
        src_path = os.path.join(src_dir, src_file)
        asm_name = src_file.replace(".pn", ".pt")
        asm_path = os.path.join(asm_dir, asm_name)
        ans_path = os.path.join(ans_dir, asm_name)

        # 出力はソース由来の日本語を含みうるため，Windows既定のcp932ではなくUTF-8で読む
        result = subprocess.run(
            [PN2ASM, "-pn", src_path, "-pt", asm_path] + extra_args,
            capture_output=True,
            encoding="utf-8",
            errors="backslashreplace",
        )

        stdout = result.stdout.strip()
        stderr = result.stderr.strip()
        output = (stdout + "\n" + stderr).strip()

        if result.returncode != 0:
            compile_fail.append((case_name, output))
            print(f"[FAIL] {case_name}: {output}")
            continue

        compile_success.append(case_name)

        # 期待値ファイルがなければエラー
        if not os.path.exists(ans_path):
            compare_fail.append(case_name)
            print(f"[FAIL] {case_name}  ({ans_name_dir}/{asm_name} が存在しません)")
            continue

        with open(asm_path, encoding="utf-8") as f:
            actual_lines = f.readlines()
        with open(ans_path, encoding="utf-8") as f:
            expected_lines = f.readlines()

        if actual_lines == expected_lines:
            compare_pass.append(case_name)
            print(f"[PASS] {case_name}")
        else:
            compare_fail.append(case_name)
            diff = difflib.unified_diff(
                expected_lines,
                actual_lines,
                fromfile=f"expected ({ans_name_dir}/{asm_name})",
                tofile=f"actual   ({asm_name_dir}/{asm_name})",
            )
            print(f"[FAIL] {case_name}")
            for line in "".join(diff).splitlines():
                print(f"  {line}")

    return True

def main():
    # まずコンパイラをビルドする
    if not build_compiler():
        sys.exit(1)

    compile_success = []
    compile_fail = []
    compare_pass = []
    compare_fail = []

    # 全ての組を実行する (テストケースのない組があっても，残りの組は実行してから失敗とする)
    all_found = True
    for suite in SUITES:
        if not run_suite(*suite, compile_success, compile_fail, compare_pass, compare_fail):
            all_found = False

    print()
    print(f"コンパイル成功: {len(compile_success)}件 / 失敗: {len(compile_fail)}件")
    print(f"期待値比較  PASS: {len(compare_pass)}件 / FAIL: {len(compare_fail)}件")

    if not all_found or compile_fail or compare_fail:
        sys.exit(1)

if __name__ == "__main__":
    main()
