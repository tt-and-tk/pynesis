"""
test/src_err/*.pn・test/src_err_bin/*.pn を全てコンパイルし，コンパイラが期待どおりのコンパイルエラーを報告することを確認するテストスクリプト．
src_err/ はROM用，src_err_bin/ はQosmosの実行ファイル用(--bin-mode)としてコンパイルする．
- 各ファイルの1行目に「// expect: <期待するメッセージ>」の形で期待するエラーメッセージを書く．
- 終了コードが1，かつ出力が期待するメッセージと完全に一致した場合を「成功(エラー検出)」とする．
  コンパイルエラーが出たかだけでは，確かめたい誤りとは別の誤りで失敗した場合も成功とみなしてしまうため，メッセージ全体を照合する．
- 出力に含まれるファイルパスは，実行する場所によらず比較できるよう，testディレクトリからの相対パス(src_err/01.pn など)に直してから比較する．
- それ以外は「失敗」とし，次の理由を区別して報告する．
  - 終了コードが0: コンパイラがエラーを検出しなかった
  - 終了コードが0・1以外: クラッシュなどによる異常終了
  - 終了コードが1だが"compiler error:"で始まる行がない: エラーメッセージの出力漏れ
  - 1行目に期待するメッセージがない，または出力が期待するメッセージと一致しない: 期待と異なるエラー
"""

import os
import subprocess
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
COMPILER_DIR = os.path.dirname(SCRIPT_DIR)
PN2ASM = os.path.join(COMPILER_DIR, "pn2asm.exe")
ERROR_RETURNCODE = 1                    # コンパイルエラー時の終了コード
ERROR_PREFIX = "compiler error:"        # コンパイルエラー時に出力される行の接頭辞
TEST_DIR_PREFIX = SCRIPT_DIR.replace(os.sep, "/") + "/"  # 出力中のパスのうちtestディレクトリまでの部分(コンパイラはパス区切りを/で出力する)
EXPECT_PREFIX = "// expect: "           # 期待するエラーメッセージを書く1行目の接頭辞

# テストの組: (入力ディレクトリ, コンパイラへ追加で渡す引数)
SUITES = [
    ("src_err", []),
    ("src_err_bin", ["--bin-mode"]),
]

def read_expected(src_path):
    """ソースファイルの1行目に書かれた期待するエラーメッセージを返す．書かれていなければ None を返す．"""
    # 期待するメッセージはソース由来の日本語を含みうるため，UTF-8で読む
    with open(src_path, encoding="utf-8") as f:
        first_line = f.readline().rstrip("\r\n")
    if not first_line.startswith(EXPECT_PREFIX):
        return None
    return first_line[len(EXPECT_PREFIX):].strip()

def relativize(output):
    """
    出力中のファイルパスを，testディレクトリからの相対パスに直して返す．
    取り込んだファイルでのエラーはパスを2つ含むことがあるため，先頭だけでなく全て置き換える．
    """
    return output.replace(TEST_DIR_PREFIX, "")

def judge(returncode, output, expected):
    """
    コンパイル結果を期待するメッセージ(書かれていなければ None)と照らして判定し，成功なら None，失敗なら失敗理由を返す．
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
    # 期待するメッセージがなく，意図したエラーかどうかを確かめられない
    if expected is None:
        return f"1行目に期待するメッセージ({EXPECT_PREFIX}...)がない"
    # 確かめたい誤りとは別の誤りでエラーになった
    if output != expected:
        return "期待と異なるエラーが出力された"
    return None

def run_suite(src_name, extra_args, tmpdir, detected, failed):
    """1組のテストを実行し，結果を各リストへ追加する．入力ディレクトリかテストケースがなければ False を返す．"""
    src_dir = os.path.join(SCRIPT_DIR, src_name)
    if not os.path.isdir(src_dir):
        print(f"{src_name} ディレクトリが見つかりません: {src_dir}")
        return False

    src_files = sorted(
        f for f in os.listdir(src_dir) if f.endswith(".pn")
    )

    if not src_files:
        print(f"異常系テストケースが見つかりません。({src_name}/)")
        return False

    for src_file in src_files:
        # 結果の表示では，どの組のテストかが分かるようディレクトリ名を付ける
        case_name = f"{src_name}/{src_file}"
        src_path = os.path.join(src_dir, src_file)
        # 組が違えば同じ連番のファイルがありうるため，出力先を組ごとに分ける
        asm_path = os.path.join(tmpdir, src_name + "_" + os.path.splitext(src_file)[0] + ".pt")

        # 出力はソース由来の日本語を含みうるため，Windows既定のcp932ではなくUTF-8で読む
        result = subprocess.run(
            [PN2ASM, "-pn", src_path, "-pt", asm_path] + extra_args,
            capture_output=True,
            encoding="utf-8",
            errors="backslashreplace",
        )

        stdout = result.stdout.strip()
        stderr = result.stderr.strip()
        # 実行する場所によらず期待するメッセージと比較できるよう，パスをtestディレクトリからの相対パスにする
        output = relativize((stdout + "\n" + stderr).strip())

        # 1行目に書かれた期待するメッセージを読む(書かれていなければ None)
        expected = read_expected(src_path)

        # 終了コードとエラーメッセージから，期待どおりのコンパイルエラーとして報告されたかを判定する
        reason = judge(result.returncode, output, expected)

        if reason is None:
            detected.append((case_name, output))
            print(f"[OK]   {case_name}: エラー検出 ({output})")
        else:
            failed.append((case_name, output))
            print(f"[FAIL] {case_name}: {reason} (returncode={result.returncode})")
            print(f"  expected: {expected!r}")
            print(f"  actual:   {output!r}")
    return True

def main():
    detected = []    # エラー検出成功(コンパイラが期待どおりのコンパイルエラーを報告した)
    failed = []      # 失敗(エラー未検出・異常終了・エラーメッセージの出力漏れ・期待と異なるエラー)

    # 全ての組を実行する (テストケースのない組があっても，残りの組は実行してから失敗とする)
    all_found = True
    with tempfile.TemporaryDirectory() as tmpdir:
        for src_name, extra_args in SUITES:
            if not run_suite(src_name, extra_args, tmpdir, detected, failed):
                all_found = False

    print()
    print(f"成功(エラー検出): {len(detected)} 件 / 失敗: {len(failed)} 件")

    if not all_found or failed:
        sys.exit(1)

if __name__ == "__main__":
    main()
