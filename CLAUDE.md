# コンパイラプロジェクト

## このリポジトリについて

GitHubリポジトリ名: `tt-and-tk/pynesis`．

PYNQ-Z2(Zynq-7000)上に実装する自作CPUと，それを動かすソフトウェア群(アセンブラ・コンパイラ・OS)からなる自作PCプロジェクトの一部．プロジェクト全体は以下の独立したGitHubリポジトリで構成される．

| リポジトリ(GitHub) | ディレクトリ(`pc/`配下) | 役割 |
|:-|:-|:-|
| `specification` | `specification/` | CPUアーキテクチャ・ISA・アセンブリ言語・コンパイラ・Qosmosの仕様のドキュメント(唯一の一次情報源) |
| `pyntaxis` | `assembler/` | 自作アセンブリ言語Pyntaxis(`.pt`) → SystemVerilog ROM(`.sv`)，または自作OS Qosmosの実行ファイルへのアセンブラ |
| `pynesis`(本リポジトリ) | `compiler/` | 自作プログラミング言語Pynesis(`.pn`) → アセンブリ言語Pyntaxisへのコンパイラ．`pyntaxis`のソースファイルをincludeして使用し，`.sv`または実行ファイルまで一貫変換も可能 |
| `qurge` | `mypc/` | CPU・メモリ・ROM等のハードウェア全体のVivadoプロジェクト(SystemVerilog + PS側C++)と，ROM上で動く自作OS Qosmos(シェルやファイルシステムなど．Pynesisで記述．仕様は`specification`の`qosmos.md`) |
| `for-pynthesis-skills` | `for-pynthesis-skills/` | 上記各リポジトリで共有するissue起票・対応支援スキルを提供する．特定のリポジトリが主担当と判断できない，全リポジトリに影響するissueの起票先(受け皿)でもある |

```
入力(.pn) → [pynesis(本リポジトリ)のコンパイラ] → アセンブリ(.pt) → [pyntaxisのアセンブラ] → SystemVerilog ROM(.sv) → [Vivado] → PYNQ-Z2上のハードウェア(qurge)
```

`pn2mc.cpp`(`pn2mc.exe`)が，Pynesisソースから`.sv`またはQosmosの実行ファイルまで一貫して変換する今後の入口となる．  
内部では`pn2asm.cpp`(Pynesisソース→アセンブリ)と`../assembler/asm2mc.cpp`(アセンブリ→`.sv`または実行ファイル)の本処理をそれぞれ`main`から分離した関数(`compile_pn_to_asm`，`assemble_asm_to_mc`)として直接リンクし，順に呼び出す(サブプロセス起動はしない)．  
`pn2asm.exe`・`../assembler/asm2mc.exe`は単体の実行ファイルとしても引き続き動作する．  
**このプロジェクトのテスト対象は`pn2asm.cpp`(アセンブリ生成まで)のままとする**．`pn2mc`はビルド確認のみで自動テストの対象外．

CLIフラグ(3ツール共通で`-pt`がアセンブリファイルを指す):
- `pn2asm.exe`: `-pn`(入力Pynesisファイル) `-pt`(出力アセンブリファイル，省略時は`.pn`から自動導出) `--bin-mode`(値を取らない．指定するとQosmosの実行ファイル用のアセンブリを出力する)
- `asm2mc.exe`: `-pt`(入力アセンブリファイル)と，出力先として`-sv`(出力`.sv`ファイル)・`-bin`(出力するQosmosの実行ファイル)のどちらか一方．どちらも省略すると，`.pt`から名前を自動導出した`.sv`を出力する
- `pn2mc.exe`: `-pn`(入力Pynesisファイル) `-pt`(中間アセンブリファイル，必須)と，出力先として`-sv`(出力`.sv`ファイル)・`-bin`(出力するQosmosの実行ファイル)のどちらか一方．どちらも省略すると，`.pt`から名前を自動導出した`.sv`を出力する．`-bin`を指定すると，実行ファイル用のアセンブリを生成する．引数の誤りは変換を始める前に検出し，中間ファイルを書き出さずにエラーになる

`pn2mc.exe`のビルドには，`pn2asm.cpp`・`../assembler/asm2mc.cpp`それぞれの`main`定義を無効化するマクロ(`PN2ASM_NO_MAIN`・`ASM2MC_NO_MAIN`)を指定し，両者の本処理ソースを`pn2mc.cpp`と一緒にコンパイルする．
```
g++ -std=c++17 -DPN2ASM_NO_MAIN -DASM2MC_NO_MAIN -o pn2mc.exe pn2mc.cpp pn2asm.cpp lexer.cpp parser.cpp analyzer.cpp generator.cpp ../assembler/asm2mc.cpp
```

## コーディング規約

`.claude/coding_conventions.md` に従うこと．  
アセンブラ(`../assembler/`)のコードを実装の参考にすること．

## Issue対応の徹底

ファイルを修正する場合は，必ず対応するGitHub issueを起票し，そのissue用のブランチ(`fix/issue-<番号>-<内容を表す短い語句>`)を作成してから行う．デフォルトブランチを直接編集しない．

## セッション一覧

| セッション名 | 役割 |
|:-|:-|
| consider | 仕様検討．対応するC言語機能の範囲，コンパイラの設計方針を決定する |
| develop | 開発．コンパイラの実装・テストを行う |

## テスト方法

アセンブラ(`../assembler/CLAUDE.md`)のテスト方針を踏襲する．

1. テスト用のPynesisソースファイルを用意する
2. 期待値を用意する．正常系は期待値のアセンブリファイルを手動で用意する．異常系は期待するエラーメッセージをソースの1行目に`// expect: <メッセージ>`の形で書く．メッセージは手で書かず`test_err.py`が表示する実際の出力(ファイルパスをtestディレクトリからの相対パスに直したもの)から作り，続くコメントに書いた意図と合っているかを確かめる
3. コンパイラで翻訳した結果が期待値と一致するか確認する

### テスト用ディレクトリ

| パス | 内容 |
|:-|:-|
| `test/src/` | 入力Pynesisソースファイル(`NN.pn`，正常系．ROM用としてコンパイルする) |
| `test/src/include/` | 正常系のソースファイルが`#include`で取り込むファイル(`NN_<役割>.pn`．単独ではコンパイルしない) |
| `test/asm/` | コンパイラの出力アセンブリ(`NN.pt`，自動生成) |
| `test/asm_ans/` | 期待値アセンブリ(`NN.pt`，手動作成) |
| `test/src_bin/`・`test/asm_bin/`・`test/asm_bin_ans/` | Qosmosの実行ファイル用(`--bin-mode`)としてコンパイルする正常系の入力・出力・期待値．役割は`src/`・`asm/`・`asm_ans/`と同じで，連番は独立 |
| `test/test.py` | 正常系テストスクリプト |
| `test/src_err/` | 異常系Pynesisソースファイル(`NN.pn`．1行目に書いたメッセージのコンパイルエラーになることを確認する．正常系`src/`とは独立した連番) |
| `test/src_err/include/` | 異常系のソースファイルが`#include`で取り込むファイル(`NN_<役割>.pn`．単独ではコンパイルしない) |
| `test/src_err_bin/` | Qosmosの実行ファイル用(`--bin-mode`)としてコンパイルする異常系の入力．役割は`src_err/`と同じで，連番は独立 |
| `test/test_err.py` | 異常系テストスクリプト |

### 実行方法

- 正常系: `test/`で`python test.py`を実行する．コンパイラをビルドし，`src/`(ROM用)・`src_bin/`(実行ファイル用)の全`.pn`をそれぞれ`asm/`・`asm_bin/`に変換して，`asm_ans/`・`asm_bin_ans/`の期待値と比較する．
- 異常系: `test/`で`python test_err.py`を実行する．`src_err/`(ROM用)・`src_err_bin/`(実行ファイル用)の全`.pn`をコンパイルし，全て1行目に書いたメッセージと一致するコンパイルエラー(終了コード1)になることを確認する．出力中のファイルパスは，testディレクトリからの相対パスに直してから比較する．クラッシュなど他の理由による終了や，期待と異なるエラーは失敗とする．

## 開発フロー

1. `pn2asm`がPynesisソースファイルをアセンブリファイル(`.pt`)に翻訳する
2. `asm2mc`(アセンブラ)がアセンブリファイルをSystemVerilog ROMファイル(`.sv`)またはQosmosの実行ファイルに変換する
3. `pn2mc`は上記1・2を順に呼び出し，Pynesisソースから`.sv`または実行ファイルまで一貫して変換する
4. テストでは`pn2asm`の翻訳結果(アセンブリ)が期待値と一致するか確認する(`pn2mc`はテスト対象外)
