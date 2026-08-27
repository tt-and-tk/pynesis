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
| `for-pynthesis-skills` | `for-pynthesis-skills/` | 上記各リポジトリで共有するissue起票・対応支援スキルを提供する．特定のリポジトリが主担当と判断できない，全リポジトリに影響するissueの起票先(受け皿)でもある |

```
入力(.pn) → [pynesis(本リポジトリ)のコンパイラ] → アセンブリ(.pt) → [pyntaxisのアセンブラ] → SystemVerilog ROM(.sv) → [Vivado] → PYNQ-Z2上のハードウェア(qurge)
```

`c2sv.cpp`(`c2sv.exe`)が，Pynesisソースから`.sv`まで一貫して変換する今後の入口となる．  
内部では`c2asm.cpp`(Pynesisソース→アセンブリ)と`../assembler/asm2sv.cpp`(アセンブリ→`.sv`)の本処理をそれぞれ`main`から分離した関数(`compile_c_to_asm`，`assemble_asm_to_sv`)として直接リンクし，順に呼び出す(サブプロセス起動はしない)．  
`c2asm.exe`・`../assembler/asm2sv.exe`は単体の実行ファイルとしても引き続き動作する．  
**このプロジェクトのテスト対象は`c2asm.cpp`(アセンブリ生成まで)のままとする**．`c2sv`はビルド確認のみで自動テストの対象外．

CLIフラグ(3ツール共通で`-pt`がアセンブリファイルを指す):
- `c2asm.exe`: `-pn`(入力Pynesisファイル) `-pt`(出力アセンブリファイル，省略時は`.pn`から自動導出)
- `asm2sv.exe`: `-pt`(入力アセンブリファイル) `-sv`(出力`.sv`ファイル，省略時は`.pt`から自動導出)
- `c2sv.exe`: `-pn` `-pt` `-sv` の3つ全てを指定する(自動導出はサポートしない．省略すると内部でエラーになる)

`c2sv.exe`のビルドには，`c2asm.cpp`・`../assembler/asm2sv.cpp`それぞれの`main`定義を無効化するマクロ(`C2ASM_NO_MAIN`・`ASM2BIN_NO_MAIN`)を指定し，両者の本処理ソースを`c2sv.cpp`と一緒にコンパイルする．`ASM2BIN_NO_MAIN`は，`asm2sv.cpp`側のプリプロセッサガード名が改名されていないためこの名前のまま使う．
```
g++ -std=c++17 -DC2ASM_NO_MAIN -DASM2BIN_NO_MAIN -o c2sv.exe c2sv.cpp c2asm.cpp lexer.cpp parser.cpp analyzer.cpp generator.cpp ../assembler/asm2sv.cpp
```

## コーディング規約

`.claude/coding_conventions.md` に従うこと．  
アセンブラ(`../assembler/`)のコードを実装の参考にすること．

## Issue対応の徹底

ファイルを修正する場合は，必ず対応するGitHub issueを起票し，そのissue用のブランチ(`fix/issue-<番号>-<内容を表す短い語句>`)を作成してから行う．デフォルトブランチを直接編集しない．

**例外:** `CLAUDE.md`や`.claude/skills/`配下のスキル定義ファイルの修正は，ソースコードの変更ではないためissue起票は不要．ただしブランチ作成は必要(デフォルトブランチを直接編集しない)．作業中の既存ブランチがあれば，新たにブランチを切らずそれに乗せてよい．

## セッション一覧

| セッション名 | 役割 |
|:-|:-|
| consider | 仕様検討．対応するC言語機能の範囲，コンパイラの設計方針を決定する |
| develop | 開発．コンパイラの実装・テストを行う |

## テスト方法

アセンブラ(`../assembler/CLAUDE.md`)のテスト方針を踏襲する．

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
2. `asm2sv`(アセンブラ)がアセンブリファイルをSystemVerilog ROMファイル(`.sv`)に変換する
3. `c2sv`は上記1・2を順に呼び出し，Pynesisソースから`.sv`まで一貫して変換する
4. テストでは`c2asm`の翻訳結果(アセンブリ)が期待値と一致するか確認する(`c2sv`はテスト対象外)
