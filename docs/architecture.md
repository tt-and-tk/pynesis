# コンパイラ内部の実装

コンパイラのソースファイル分割方針を説明する。言語仕様・コンパイラ設計上の制約そのものは`../specification/compiler.md`・`../specification/limitations.md`を参照(このファイルには記載しない)。

## ソースファイル構成

コンパイラのパイプライン段階ごとに1ファイルペアを割り当てる。「宣言する型」列は型のみを対象とし，各ヘッダが宣言する定数(`MAX_CALL_DEPTH`等)はここには挙げていない(該当ヘッダを直接参照)。

| ファイル | 担当 | 宣言する型 |
|:-|:-|:-|
| `lexer.hpp` / `lexer.cpp` | 字句解析 | `token_kind_t`，`token_t` |
| `parser.hpp` / `parser.cpp` | 構文解析(`Parser`クラス) | `base_type_t`，`type_t`，`node_kind_t`，`node_t` |
| `analyzer.hpp` / `analyzer.cpp` | 意味解析 | `symbol_t`，`location_t`，`struct_member_t`，`struct_def_t` |
| `generator.hpp` / `generator.cpp` | コード生成 | (なし) |
| `c2asm.hpp` / `c2asm.cpp` | main・引数処理・パイプライン接続 | `args_t` (ファイル内のみ) |
| `c2bin.cpp` | Pynesisソース→アセンブリ→`.sv`まで一貫して変換する入口。`c2asm.hpp`と`../assembler/asm2bin_main.hpp`(`assemble_asm_to_sv`を宣言)をincludeし，`compile_c_to_asm`と`assemble_asm_to_sv`を順に呼ぶだけ | (なし) |

設計原則:

- 段階間のインターフェース型は**生成する側のヘッダ**に置く(トークンはLexerが作るので`lexer.hpp`，ASTはParserが作るので`parser.hpp`)
- includeの依存はパイプラインの流れと一致する(`lexer.hpp ← parser.hpp ← analyzer.hpp ← generator.hpp`)
- 段階内でしか使わない型・テーブルはhppに置かずcpp内に置く
