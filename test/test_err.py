"""
test/src_err/*.pn を全てコンパイルし，エラーが出ることを確認するテストスクリプト。
- 終了コードが非0、またはエラーメッセージが出力されることを「成功（エラー検出）」とする。
- エラーが出なかった場合は「失敗（エラー未検出）」として報告する。
"""

import os
import subprocess
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
COMPILER_DIR = os.path.dirname(SCRIPT_DIR)
SRC_ERR_DIR = os.path.join(SCRIPT_DIR, "src_err")
PN2ASM = os.path.join(COMPILER_DIR, "pn2asm.exe")

def main():
    if not os.path.isdir(SRC_ERR_DIR):
        print(f"src_err ディレクトリが見つかりません: {SRC_ERR_DIR}")
        sys.exit(1)

    src_files = sorted(
        f for f in os.listdir(SRC_ERR_DIR) if f.endswith(".pn")
    )

    if not src_files:
        print("異常系テストケースが見つかりません。")
        sys.exit(1)

    detected = []    # エラー検出成功（コンパイラが正しくエラーを返した）
    undetected = []  # エラー未検出（コンパイラが誤って正常終了した）

    with tempfile.TemporaryDirectory() as tmpdir:
        for src_file in src_files:
            src_path = os.path.join(SRC_ERR_DIR, src_file)
            asm_path = os.path.join(tmpdir, src_file.replace(".pn", ".pt"))

            result = subprocess.run(
                [PN2ASM, "-pn", src_path, "-pt", asm_path],
                capture_output=True,
                text=True,
            )

            stdout = result.stdout.strip()
            stderr = result.stderr.strip()
            output = (stdout + "\n" + stderr).strip()

            # 終了コード非0 または エラーメッセージが含まれていればエラー検出成功
            error_detected = result.returncode != 0 or (
                "error" in output.lower() or "fail" in output.lower()
            )

            if error_detected:
                detected.append((src_file, output))
                print(f"[OK]   {src_file}: エラー検出 ({output})")
            else:
                undetected.append((src_file, output))
                print(f"[FAIL] {src_file}: エラーが検出されなかった (returncode={result.returncode}, output={output!r})")

    print()
    print(f"成功（エラー検出）: {len(detected)} 件 / 失敗（エラー未検出）: {len(undetected)} 件")

    if undetected:
        sys.exit(1)

if __name__ == "__main__":
    main()
