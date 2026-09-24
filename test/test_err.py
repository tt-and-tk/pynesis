"""
test/src_err/*.pn を全てコンパイルし，コンパイラがコンパイルエラーを報告することを確認するテストスクリプト．
- 終了コードが1，かつ"compiler error:"で始まる行が出力された場合を「成功(エラー検出)」とする．
- それ以外は「失敗」とし，次の理由を区別して報告する．
  - 終了コードが0: コンパイラがエラーを検出しなかった
  - 終了コードが0・1以外: クラッシュなどによる異常終了
  - 終了コードが1だが"compiler error:"で始まる行がない: エラーメッセージの出力漏れ
"""

import os
import subprocess
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
COMPILER_DIR = os.path.dirname(SCRIPT_DIR)
SRC_ERR_DIR = os.path.join(SCRIPT_DIR, "src_err")
PN2ASM = os.path.join(COMPILER_DIR, "pn2asm.exe")
ERROR_RETURNCODE = 1                    # コンパイルエラー時の終了コード
ERROR_PREFIX = "compiler error:"        # コンパイルエラー時に出力される行の接頭辞

def judge(returncode, output):
    """
    コンパイル結果を判定し，成功なら None，失敗なら失敗理由を返す．
    引数不正時の"args fail"は，常に正しい引数を渡すこのテストでは不具合の兆候であるため成功扱いにしない．
    """
    # コンパイラがエラーを検出せず正常終了した
    if returncode == 0:
        return "エラーが検出されなかった"
    # クラッシュなど，コンパイルエラー以外の理由で終了した
    if returncode != ERROR_RETURNCODE:
        return "異常終了した"
    # コンパイルエラーの終了コードだが，エラーメッセージが出力されていない
    if not any(line.startswith(ERROR_PREFIX) for line in output.splitlines()):
        return "エラーメッセージが出力されなかった"
    return None

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

    detected = []    # エラー検出成功(コンパイラが正しくコンパイルエラーを報告した)
    failed = []      # 失敗(エラー未検出・異常終了・エラーメッセージの出力漏れ)

    with tempfile.TemporaryDirectory() as tmpdir:
        for src_file in src_files:
            src_path = os.path.join(SRC_ERR_DIR, src_file)
            asm_path = os.path.join(tmpdir, os.path.splitext(src_file)[0] + ".pt")

            # 出力はソース由来の日本語を含みうるため，Windows既定のcp932ではなくUTF-8で読む
            result = subprocess.run(
                [PN2ASM, "-pn", src_path, "-pt", asm_path],
                capture_output=True,
                encoding="utf-8",
                errors="backslashreplace",
            )

            stdout = result.stdout.strip()
            stderr = result.stderr.strip()
            output = (stdout + "\n" + stderr).strip()

            # 終了コードとエラーメッセージから，コンパイルエラーとして正しく報告されたかを判定する
            reason = judge(result.returncode, output)

            if reason is None:
                detected.append((src_file, output))
                print(f"[OK]   {src_file}: エラー検出 ({output})")
            else:
                failed.append((src_file, output))
                print(f"[FAIL] {src_file}: {reason} (returncode={result.returncode}, output={output!r})")

    print()
    print(f"成功(エラー検出): {len(detected)} 件 / 失敗: {len(failed)} 件")

    if failed:
        sys.exit(1)

if __name__ == "__main__":
    main()
