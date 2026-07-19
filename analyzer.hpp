#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "parser.hpp"

// ハードウェア制約: 関数呼び出しネストの最大段数 (戻り先レジスタ6'h11〜6'h1aの10本による)
const int MAX_CALL_DEPTH = 10;
// ソフトウェア側の安全上限: プログラムの最大命令数
// (ROM自体に固定容量は無く，ROM_SIZEはコンパイル対象プログラムのサイズに応じてアセンブラが自動算出する．
//  現行のROM読み出し回路は組合せ論理でLUT資源を消費するため，その範囲で安全に収まる値として設定．
//  c2asm.cppで生成後に検査する．詳細は ../specification/limitations.md を参照)
const int MAX_INSTRUCTION_COUNT = 4096;
// ハードウェア制約: 汎用レジスタの本数 (r0〜r15の16本)
const int MAX_REG = 16;

// 変数の置き場所の種別
typedef enum {
    LOC_REGISTER,   // レジスタ直結 (LED等のハードウェア変数)
    LOC_GLOBAL,     // メモリ上の絶対番地 (グローバル変数)
    LOC_LOCAL,      // 関数ローカルなメモリ領域 (現状は静的割り当ての固定番地，将来は相対アドレス)
} location_t;

// シンボル情報
struct symbol_t {
    std::string name;       // 変数名
    type_t type;            // 型情報
    location_t location;    // 置き場所の種別
    int address;            // レジスタ番地 / メモリ絶対番地 / SPオフセット (locationに応じて解釈)
    bool readable;          // 読み込み可能かどうか (falseの参照はコンパイルエラー)
    bool writable;          // 書き込み可能かどうか (falseへの代入はコンパイルエラー)
};

// 構造体メンバ1つ分の情報 (構造体先頭からのオフセットまで確定させた状態で保持する)
struct struct_member_t {
    std::string name;    // メンバ名
    type_t type;         // メンバの型 (スカラーまたは固定長配列．ネスト構造体は非対応)
    int offset_words;    // 構造体先頭からのオフセット(ワード単位)
};

// 構造体定義1つ分の情報 (構造体名→この情報がstruct_defs_に登録される)
struct struct_def_t {
    std::vector<struct_member_t> members;  // 宣言順のメンバ一覧
    int total_words;                       // 構造体全体が占めるワード数
};

// ASTを受け取り，意味検査とシンボルテーブル構築を行うアナライザ
class Analyzer {
public:
    explicit Analyzer(node_t *root);
    std::map<std::string, const symbol_t *> operator()();   // 意味解析を実行してシンボルテーブルを返す
    // パラメータシンボル表 (関数名→パラメータのシンボル列．コード生成で引数の書き込み先アドレスに使う)
    const std::map<std::string, std::vector<const symbol_t *>> &func_params() const;
    // 構造体定義表(struct_defs_)を返す単純なゲッター．
    // コード生成が，構造体配列の要素1個分が占めるバイト数(配列上で要素を飛び越す間隔)を
    // 計算するのに使う(構造体配列は，このバイト数×添字ぶんだけ先頭番地からずらして各要素の番地を求める)
    const std::map<std::string, struct_def_t> &struct_defs() const;
    // 呼び出しをまたいで生かしたいレジスタ値の退避領域の先頭番地
    // (呼び出された関数はr0から使い直すため，レジスタは呼び出しをまたいで保持されない．
    //  全変数のアドレス割り当てが終わった直後の空き番地から，MAX_REG個分の退避枠を確保している)
    int scratch_base() const;

private:
    node_t *root_;                                       // AST
    std::map<std::string, const symbol_t *> symbols_;    // シンボルテーブル (変数名→保存先番地等の対応表)
    std::map<std::string, type_t> func_names_;           // 定義済み関数名→戻り値型の対応表
    std::map<std::string, std::vector<const symbol_t *>> func_params_;  // 関数名→パラメータのシンボル列
    std::map<std::string, struct_def_t> struct_defs_;    // 構造体名→メンバ構成の対応表
    int next_addr_;                                      // 次に割り当てるメモリ番地 (グローバル→ローカルで連番)
    int scratch_base_;                                    // レジスタ退避領域の先頭番地 (全変数のアドレス割り当て後に確保)
    std::vector<std::map<std::string, const symbol_t *>> scopes_;  // ローカル変数のスコープスタック (内側ほど後ろ)
    int loop_depth_ = 0;                                 // ループの入れ子の深さ (break/continueの検査用)
    int switch_depth_ = 0;                               // switchの入れ子の深さ (breakの検査用)
    std::string current_function_;                       // 現在解析中の関数名 (呼び出しグラフ構築用)
    type_t current_return_type_;                         // 現在解析中の関数の戻り値型 (return文の整合性検査用)
    std::map<std::string, std::set<std::string>> call_graph_;  // 関数名→直接呼び出す関数名の集合 (ネスト段数検査用)

    // 解析メソッド
    void collect_struct_decls();                            // 1パス目: 構造体定義の登録 (変数のアドレス確保より前に必要)
    void collect_globals();                                 // 2パス目: グローバル変数の登録と関数名の収集
    // 定数式をコンパイル時に計算する (初期化子・配列サイズ・case値)
    // sizeof(変数名)の解決にシンボルテーブル参照が必要なため非static
    long long eval_const_expr(const node_t *expr);
    static int calc_array_words(const type_t &type);        // 配列が占有するワード数を計算する
    // 型のバイト数を返す (sizeof用．配列は要素数×要素サイズ)．構造体はstruct_defs_からメンバ構成を引いて計算する
    int type_size_bytes(const type_t &type) const;
    // 構造体型の変数1つ分(配列宣言ならその配列全体分)のアドレスを確保し，シンボル(symbol_t)を生成して返す
    // (シンボル表への格納自体は呼び出し元(collect_globals/analyze_local_decl)がグローバル/ローカルの
    //  区別に応じて行うため，この関数はシンボルの生成・アドレス確保だけに専念する．
    //  配列サイズを定数式から計算し，宣言ノードの子を計算済みの数値に置き換える(畳み込む)ため，
    //  declの中身を書き換える必要があり，読み取り専用にはできない)
    symbol_t *register_struct_var(node_t *decl, location_t location);
    void analyze_functions();                               // 3パス目: 各関数本体を検査する
    void analyze_block(node_t *block);                      // ブロックを検査する (新しいスコープを積む)
    void analyze_stmt(node_t *stmt);                        // 文を検査する
    void analyze_switch(node_t *stmt);                      // switch文を検査する
    void analyze_local_decl(node_t *decl);                  // ローカル変数宣言を検査し登録する
    void analyze_expr(node_t *expr);                        // 式を検査し名前解決・型注釈する
    const symbol_t *lookup_symbol(const std::string &name) const;  // 名前からシンボルを探す (スコープ→グローバル)
    // 呼び出しグラフを検査する (再帰の検出，最大ネスト段数MAX_CALL_DEPTHの超過検出)
    void check_call_depth();
    // mainから呼び出しグラフを深さ優先探索し，再帰(既に経路上にある関数への到達)とネスト段数を検査する
    // path: 現在の呼び出し経路(再帰検出用)．depthは戻り値(mainからのネスト段数の最大値)
    int check_call_depth_dfs(const std::string &func, std::set<std::string> &path);
};
