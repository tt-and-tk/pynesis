"""
test/src_err/*.pn を全てコンパイルし，コンパイラが期待どおりのコンパイルエラーを報告することを確認するテストスクリプト．
- 終了コードが1，かつ出力が test/err_ans/ の期待値(.pnを.txtに替えた名前)と完全に一致した場合を「成功(エラー検出)」とする．
  コンパイルエラーが出たかだけでは，確かめたい誤りとは別の誤りで失敗した場合も成功とみなしてしまうため，メッセージ全体を照合する．
- 出力に含まれるファイルパスは，実行する場所によらず比較できるよう，testディレクトリからの相対パス(src_err/01.pn など)に直してから比較する．
- それ以外は「失敗」とし，次の理由を区別して報告する．
  - 終了コードが0: コンパイラがエラーを検出しなかった
  - 終了コードが0・1以外: クラッシュなどによる異常終了
  - 終了コードが1だが"compiler error:"で始まる行がない: エラーメッセージの出力漏れ
  - 期待値ファイルがない，または出力が期待値と一致しない: 期待と異なるエラー
"""

import os
import subprocess
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
COMPILER_DIR = os.path.dirname(SCRIPT_DIR)
SRC_ERR_DIR = os.path.join(SCRIPT_DIR, "src_err")
ERR_ANS_DIR = os.path.join(SCRIPT_DIR, "err_ans")
PN2ASM = os.path.join(COMPILER_DIR, "pn2asm.exe")
ERROR_RETURNCODE = 1                    # コンパイルエラー時の終了コード
ERROR_PREFIX = "compiler error:"        # コンパイルエラー時に出力される行の接頭辞
TEST_DIR_PREFIX = SCRIPT_DIR.replace(os.sep, "/") + "/"  # 出力中のパスのうちtestディレクトリまでの部分(コンパイラはパス区切りを/で出力する)

def relativize(output):
    """
    出力中のファイルパスを，testディレクトリからの相対パスに直して返す．
    取り込んだファイルでのエラーはパスを2つ含むことがあるため，先頭だけでなく全て置き換える．
    """
    return output.replace(TEST_DIR_PREFIX, "")

def judge(returncode, output, expected):
    """
    コンパイル結果を期待値(期待値ファイルがなければ None)と照らして判定し，成功なら None，失敗なら失敗理由を返す．
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
    # 期待値がなく，意図したエラーかどうかを確かめられない
    if expected is None:
        return "期待値ファイルが存在しない"
    # 確かめたい誤りとは別の誤りでエラーになった
    if output != expected:
        return "期待と異なるエラーが出力された"
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

    detected = []    # エラー検出成功(コンパイラが期待どおりのコンパイルエラーを報告した)
    failed = []      # 失敗(エラー未検出・異常終了・エラーメッセージの出力漏れ・期待と異なるエラー)

    with tempfile.TemporaryDirectory() as tmpdir:
        for src_file in src_files:
            src_path = os.path.join(SRC_ERR_DIR, src_file)
            asm_path = os.path.join(tmpdir, os.path.splitext(src_file)[0] + ".pt")
            ans_path = os.path.join(ERR_ANS_DIR, os.path.splitext(src_file)[0] + ".txt")

            # 出力はソース由来の日本語を含みうるため，Windows既定のcp932ではなくUTF-8で読む
            result = subprocess.run(
                [PN2ASM, "-pn", src_path, "-pt", asm_path],
                capture_output=True,
                encoding="utf-8",
                errors="backslashreplace",
            )

            stdout = result.stdout.strip()
            stderr = result.stderr.strip()
            # 実行する場所によらず期待値と比較できるよう，パスをtestディレクトリからの相対パスにする
            output = relativize((stdout + "\n" + stderr).strip())

            # err_ans/ の期待値を読む(期待値ファイルがなければ None)
            expected = None
            if os.path.exists(ans_path):
                with open(ans_path, encoding="utf-8") as f:
                    expected = f.read().strip()

            # 終了コードとエラーメッセージから，期待どおりのコンパイルエラーとして報告されたかを判定する
            reason = judge(result.returncode, output, expected)

            if reason is None:
                detected.append((src_file, output))
                print(f"[OK]   {src_file}: エラー検出 ({output})")
            else:
                failed.append((src_file, output))
                print(f"[FAIL] {src_file}: {reason} (returncode={result.returncode})")
                print(f"  expected: {expected!r}")
                print(f"  actual:   {output!r}")

    print()
    print(f"成功(エラー検出): {len(detected)} 件 / 失敗: {len(failed)} 件")

    if failed:
        sys.exit(1)

if __name__ == "__main__":
    main()
