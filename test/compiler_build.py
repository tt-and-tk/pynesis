"""
正常系・異常系のテストスクリプトが共通で使う，テスト対象のコンパイラをビルドする処理．
どちらのスクリプトも単独で実行でき，常に最新のソースからビルドしたコンパイラでテストするよう，各スクリプトの最初に呼び出す．
"""

import os
import subprocess

COMPILER_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # コンパイラのソースがあるディレクトリ
PN2ASM = os.path.join(COMPILER_DIR, "pn2asm.exe")                           # ビルドしたコンパイラの実行ファイル
SOURCES = ["lexer.cpp", "parser.cpp", "analyzer.cpp", "generator.cpp", "pn2asm.cpp"]  # ビルド対象のソース一式

def build_compiler():
    """コンパイラをビルドする．成功すれば True を返す．"""
    # 警告を有効にして，ソース一式から実行ファイルを作るコマンドを組み立てる
    cmd = ["g++", "-Wall", "-Wextra", "-std=c++17", "-o", PN2ASM]
    cmd += [os.path.join(COMPILER_DIR, s) for s in SOURCES]
    # g++の警告はソース中の日本語コメントを引用するため，Windows既定のcp932ではなくUTF-8で読む
    result = subprocess.run(cmd, capture_output=True, encoding="utf-8", errors="backslashreplace")
    # ビルドに失敗した場合は，原因が分かるようg++のエラー出力を表示する
    if result.returncode != 0:
        print("[BUILD FAIL] コンパイラのビルドに失敗しました")
        print(result.stderr.strip())
        return False
    return True
