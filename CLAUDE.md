# コンパイラプロジェクト

## このリポジトリについて

GitHubリポジトリ名: `tt-and-tk/pynesis`．

PYNQ-Z2(Zynq-7000)上に実装する自作CPUと，それを動かすソフトウェア群(アセンブラ・コンパイラ)からなる自作PCプロジェクトの一部．プロジェクト全体は以下の独立したGitHubリポジトリで構成される．

| リポジトリ(GitHub) | ディレクトリ(`pc/`配下) | 役割 |
|:-|:-|:-|
| `specification` | `specification/` | CPUアーキテクチャ・ISA・アセンブリ言語・コンパイラ仕様のドキュメント(唯一の一次情報源) |
| `pyntaxis` | `assembler/` | 自作アセンブリ言語Pyntaxis(`.pt`) → SystemVerilog ROM(`.sv`)へのアセンブラ |
| `pynesis`(本リポジトリ) | `compiler/` | 自作プログラミング言語Pynesis(`.pn`) → アセンブリ言語Pyntaxisへのコンパイラ．`pyntaxis`のソースファイルをincludeして使用し，`.sv`まで一貫変換も可能 |
| `qurge` | `mypc/` | CPU・メモリ・ROM等のハードウェア全体のVivadoプロジェクト(SystemVerilog + PS側C++) |

```
入力(.pn) → [pynesis(本リポジトリ)のコンパイラ] → アセンブリ(.pt) → [pyntaxisのアセンブラ] → SystemVerilog ROM(.sv) → [Vivado] → PYNQ-Z2上のハードウェア(qurge)
```

`c2bin.cpp`(`c2bin.exe`)が，Pynesisソースから`.sv`まで一貫して変換する今後の入口となる．  
内部では`c2asm.cpp`(Pynesisソース→アセンブリ)と`../assembler/asm2bin.cpp`(アセンブリ→`.sv`)の本処理をそれぞれ`main`から分離した関数(`compile_c_to_asm`，`assemble_asm_to_sv`)として直接リンクし，順に呼び出す(サブプロセス起動はしない)．  
`c2asm.exe`・`../assembler/asm2bin.exe`は単体の実行ファイルとしても引き続き動作する．  
**このプロジェクトのテスト対象は`c2asm.cpp`(アセンブリ生成まで)のままとする**．`c2bin`はビルド確認のみで自動テストの対象外．

CLIフラグ(3ツール共通で`-pt`がアセンブリファイルを指す):
- `c2asm.exe`: `-pn`(入力Pynesisファイル) `-pt`(出力アセンブリファイル，省略時は`.pn`から自動導出)
- `asm2bin.exe`: `-pt`(入力アセンブリファイル) `-bin`(出力`.sv`ファイル，省略時は`.pt`から自動導出)
- `c2bin.exe`: `-pn` `-pt` `-bin` の3つ全てを指定する(自動導出はサポートしない．省略すると内部でエラーになる)

`c2bin.exe`のビルドには，`c2asm.cpp`・`../assembler/asm2bin.cpp`それぞれの`main`定義を無効化するマクロ(`C2ASM_NO_MAIN`・`ASM2BIN_NO_MAIN`)を指定し，両者の本処理ソースを`c2bin.cpp`と一緒にコンパイルする．
```
g++ -std=c++17 -DC2ASM_NO_MAIN -DASM2BIN_NO_MAIN -o c2bin.exe c2bin.cpp c2asm.cpp lexer.cpp parser.cpp analyzer.cpp generator.cpp ../assembler/asm2bin.cpp
```

## ソースファイル構成

コンパイラのパイプライン段階ごとに1ファイルペアを割り当てる．

| ファイル | 担当 | 宣言する型 |
|:-|:-|:-|
| `lexer.hpp` / `lexer.cpp` | 字句解析 | `token_kind_t`，`token_t` |
| `parser.hpp` / `parser.cpp` | 構文解析(`Parser`クラス) | `base_type_t`，`type_t`，`node_kind_t`，`node_t` |
| `analyzer.hpp` / `analyzer.cpp` | 意味解析 | `symbol_t` |
| `generator.hpp` / `generator.cpp` | コード生成 | (なし) |
| `c2asm.hpp` / `c2asm.cpp` | main・引数処理・パイプライン接続 | `args_t` (ファイル内のみ) |
| `c2bin.cpp` | Pynesisソース→アセンブリ→`.sv`まで一貫して変換する入口(`compile_c_to_asm`と`assemble_asm_to_sv`を順に呼ぶだけ) | (なし) |

設計原則:

- 段階間のインターフェース型は**生成する側のヘッダ**に置く(トークンはLexerが作るので`lexer.hpp`，ASTはParserが作るので`parser.hpp`)
- includeの依存はパイプラインの流れと一致する(`lexer.hpp ← parser.hpp ← analyzer.hpp ← generator.hpp`)
- 段階内でしか使わない型・テーブルはhppに置かずcpp内に置く

## コーディング規約

`.claude/coding_conventions.md` に従うこと．  
アセンブラ(`../assembler/`)のコードを実装の参考にすること．

## Issue対応の徹底

ファイルを修正する場合は，必ず対応するGitHub issueを起票し，そのissue用のブランチ(`fix/issue-<番号>-<内容を表す短い語句>`)を作成してから行う．デフォルトブランチを直接編集しない．

**例外:** `CLAUDE.md`や`.claude/skills/`配下のスキル定義ファイルの修正は，ソースコードの変更ではないためissue起票は不要．ただしブランチ作成は必要(デフォルトブランチを直接編集しない)．作業中の既存ブランチがあれば，新たにブランチを切らずそれに乗せてよい．

## 自作CPUアーキテクチャ概要

コンパイラ設計に関わる主要な仕様を記載する．詳細は `../specification/` を参照．

### コンパイラ設計上の制約

- プログラム最大命令数: **4096命令**(ハードウェア制約ではなく，現行のROM読み出し回路(組合せ論理)がLUT資源のみで安全に収まる範囲として設定したソフトウェア側の暫定上限．ROM自体の容量はコンパイル対象プログラムのサイズに応じてアセンブラが自動算出するため固定値ではない．超過は`c2asm.cpp`がアセンブリ生成後に静的検査してコンパイルエラーにする．詳細は`../specification/limitations.md`を参照)
- 関数呼び出しネスト上限: **10段**(ハードウェア制約．超過・再帰呼び出しは`analyzer.cpp`の呼び出しグラフ検査(`check_call_depth`)で静的検査してコンパイルエラーにする)
- CALL/RETの戻り先管理はハードウェアが行う
- 条件分岐(F系)はPCへの**相対**オフセット加算(true時)，false時はPCをインクリメント
- JMPはレジスタ値または即値の**絶対**アドレスへジャンプ
- DIV命令は商と余りの両方を計算する
- グローバル変数は**絶対アドレス**配置(将来OSに対応する際に相対アドレスへ移行予定)
- ローカル変数も**絶対アドレス**の静的割り当て(SPが読み書き禁止でスタック構築不可)．このため**再帰呼び出しは非対応**

## 将来対応予定

現状はスモールスタートの簡易実装．以下は将来の移行・対応計画(詳細は `../specification/compiler.md`)．

| 項目 | 現状 | 将来 | 前提 |
|:-|:-|:-|:-|
| グローバル変数の配置 | 絶対アドレス | 相対アドレス | OS(プログラムのロード実行)対応 |
| ローカル変数の配置 | 絶対アドレスの静的割り当て | 相対アドレス | 同上 |
| 再帰呼び出し | 非対応 | 対応 | SP(`6'h10`)を使用可能にするハードウェア変更 |
| `long` / 配列 / 関数引数・戻り値 | 非対応 | 対応 | (compiler.mdの未対応機能表を参照) |

## セッション一覧

| セッション名 | 役割 |
|:-|:-|
| consider | 仕様検討．対応するC言語機能の範囲，コンパイラの設計方針を決定する |
| develop | 開発．コンパイラの実装・テストを行う |

## 対応するC言語機能

`../specification/compiler.md` を参照．considerセッションで決定済み．

## コンパイラの設計

`../specification/compiler.md` の概要を参照．内部設計(構文解析手法・コード生成戦略等)は開発セッションで決定する．

## テスト方法

アセンブラのテスト方針を踏襲する．

1. テスト用のPynesisソースファイルを用意する
2. 期待値のアセンブリファイルを手動で用意する
3. コンパイラで翻訳した結果が期待値と一致するか確認する

### テスト用ディレクトリ

| パス | 内容 |
|:-|:-|
| `test/src/` | 入力Pynesisソースファイル(`NN.pn`，正常系) |
| `test/asm/` | コンパイラの出力アセンブリ(`NN.pt`，自動生成) |
| `test/asm_ans/` | 期待値アセンブリ(`NN.pt`，手動作成) |
| `test/test.py` | 正常系テストスクリプト |
| `test/src_err/` | 異常系Pynesisソースファイル(`NN.pn`．コンパイルエラーになることを確認する．正常系`src/`とは独立した連番) |
| `test/test_err.py` | 異常系テストスクリプト(アセンブラの`test_err.py`と同じ方針) |

### 実行方法

- 正常系: `test/`で`python test.py`を実行する．コンパイラをビルドし，`src/`の全`.pn`を`asm/`に変換して`asm_ans/`の期待値と比較する．
- 異常系: `test/`で`python test_err.py`を実行する．`src_err/`の全`.pn`をコンパイルし，全てエラー(非0終了コードまたはエラーメッセージ)になることを確認する．

## 開発フロー

1. `c2asm`がPynesisソースファイルをアセンブリファイル(`.pt`)に翻訳する
2. `asm2bin`(アセンブラ)がアセンブリファイルをSystemVerilog ROMファイル(`.sv`)に変換する
3. `c2bin`は上記1・2を順に呼び出し，Pynesisソースから`.sv`まで一貫して変換する
4. テストでは`c2asm`の翻訳結果(アセンブリ)が期待値と一致するか確認する(`c2bin`はテスト対象外)
