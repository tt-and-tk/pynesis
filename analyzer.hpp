#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "parser.hpp"

// ハードウェア制約: メインメモリの容量(バイト)
// (グローバル変数を下位番地から，スタックを上端から確保するため，両者の合計がこの値に収まる必要がある．
//  詳細は ../specification/memory.md を参照)
const int RAM_SIZE = 65536;
// ハードウェア制約: プログラムの最大命令数
// (ROM自体に固定容量は無く，ROM_SIZEはコンパイル対象プログラムのサイズに応じてアセンブラが自動算出する．
//  プログラムカウンタのビット幅(14ビット)がちょうど表現できる命令数であり，アセンブラの上限とも揃える．
//  pn2asm.cppで生成後に検査する．詳細は ../specification/limitations.md を参照)
const int MAX_INSTRUCTION_COUNT = 16384;
// ハードウェア制約: 汎用レジスタの本数 (r0〜r15の16本)
const int MAX_REG = 16;

// 整数昇格後の型が符号付き(int)かどうかを返す
// (char/shortは符号の有無によらずintへ昇格するため，符号付きでないのはunsigned intのスカラーと，
//  番地を符号なしの値として比較するポインタ・nullptrのみ)
bool is_promoted_signed(const type_t &type);
// 値がポインタ(関数ポインタ・nullptrを含む)として扱われるかどうかを返す
// (条件式でnullptrを偽とする判定や，整数との取り違えの検査に使う)
bool is_pointer_like(const type_t &type);
// 二項演算を符号付きで行うかどうかを返す
// (シフトは左オペランドの型だけで決まり，それ以外は両方のオペランドが昇格後intなら符号付き)
bool is_signed_operation(const std::string &op, const type_t &lhs, const type_t &rhs);

// 定数式の値
struct const_value_t {
    long long value;    // 値 (符号付きなら-2147483648〜2147483647，符号なしなら0〜4294967295の範囲に折り返し済み)
    bool is_signed;     // 昇格後の型が符号付き(int)か (falseならunsigned int)
};

// 変数の置き場所の種別
typedef enum {
    LOC_REGISTER,   // レジスタ直結 (LED等のハードウェア変数)
    LOC_GLOBAL,     // メモリ上の絶対番地 (グローバル変数)
    LOC_LOCAL,      // スタックフレームのローカル変数領域 (addressはその領域の先頭からのバイトオフセット)
    LOC_PARAM,      // スタックフレームの引数領域 (addressはその領域の先頭からのバイトオフセット)
    LOC_CONST,      // 置き場所を持たないコンパイル時定数 (const変数．参照箇所へ値を直接埋め込む)
} location_t;

// シンボル情報
struct symbol_t {
    std::string name;       // 変数名
    type_t type;            // 型情報
    location_t location;    // 置き場所の種別
    int address;            // レジスタ番地 / メモリ絶対番地 / フレーム内オフセット / 定数値の32ビットのビット列 (locationに応じて解釈)
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

// 型の論理バイト数を返す (配列は要素数×要素のバイト数．構造体はstruct_defsのメンバ構成から求める)
int type_size_bytes(const type_t &type, const std::map<std::string, struct_def_t> &struct_defs);

// 意味解析の結果 (コード生成が参照する情報をまとめたもの．各表はアナライザが保持する実体を指す)
struct analysis_result_t {
    // 関数名→引数のシンボル列 (コード生成で引数の書き込み先の位置に使う)
    const std::map<std::string, std::vector<const symbol_t *>> &func_params;
    // 構造体名→メンバ構成 (コード生成が，構造体の要素1個分が占めるバイト数とメンバのオフセットを求めるのに使う．
    //  構造体配列は，このバイト数×添字ぶんだけ先頭からずらして各要素の位置を求める)
    const std::map<std::string, struct_def_t> &struct_defs;
    // 関数名→ローカル変数領域のバイト数 (コード生成がスタックフレームの大きさを決めるのに使う)
    const std::map<std::string, int> &func_local_sizes;
    // 関数名→呼び出しうる関数名の集合 (コード生成が最大スタック使用量を求めるのに使う．
    //  関数ポインタを通した呼び出しは呼び出し先が実行時に決まるため，番地を取得された全関数を呼びうるものとして含める)
    const std::map<std::string, std::set<std::string>> &call_graph;
    int global_size;   // グローバル変数・文字列リテラルが占めるバイト数
};

// ASTを受け取り，意味検査とシンボルテーブル構築を行うアナライザ
class Analyzer {
public:
    explicit Analyzer(node_t *root);
    std::map<std::string, const symbol_t *> operator()();   // 意味解析を実行してシンボルテーブルを返す
    analysis_result_t result() const;   // コード生成が参照する解析結果を返す

private:
    node_t *root_;                                       // AST

    // 解析の成果物 (result()でコード生成へ渡す)
    std::map<std::string, const symbol_t *> symbols_;    // シンボルテーブル (変数名→保存先番地等の対応表)
    std::map<std::string, std::vector<const symbol_t *>> func_params_;  // 関数名→引数のシンボル列
    std::map<std::string, struct_def_t> struct_defs_;    // 構造体名→メンバ構成の対応表
    std::map<std::string, int> func_local_sizes_;        // 関数名→ローカル変数領域のバイト数
    std::map<std::string, std::set<std::string>> call_graph_;  // 関数名→呼び出しうる関数名の集合
    int next_addr_;                                      // 次に割り当てるグローバル変数の絶対番地 (割り当て後はグローバル領域のバイト数)

    // 解析の途中で使う情報
    std::map<std::string, type_t> func_names_;           // 定義済み関数名→戻り値型の対応表
    std::map<std::string, std::shared_ptr<func_sig_t>> func_sigs_;  // 関数名→シグネチャ (関数の番地の型と呼び出しの引数検査に使う)
    std::set<std::string> addr_taken_funcs_;             // 番地を取得された関数名 (関数ポインタを通して呼ばれうる関数)
    std::set<std::string> indirect_callers_;             // 関数ポインタを通した呼び出しを含む関数(呼び出し元)の名前
    std::map<std::string, node_t *> global_var_decls_;   // グローバル変数名(const変数を含む)→宣言ノード (宣言順によらず型・値を解決する)
    std::map<std::string, node_t *> struct_decl_nodes_;  // 構造体名→構造体定義ノード (宣言順によらずメンバ構成を解決する)
    std::set<const node_t *> resolving_decls_;           // 型・値を解決中の宣言ノード (循環参照の検出用)
    int local_size_;                                     // 現在解析中の関数のローカル変数領域に確保済みのバイト数
    std::vector<std::map<std::string, const symbol_t *>> scopes_;  // ローカル変数のスコープスタック (内側ほど後ろ)
    int loop_depth_ = 0;                                 // ループの入れ子の深さ (break/continueの検査用)
    int switch_depth_ = 0;                               // switchの入れ子の深さ (breakの検査用)
    std::string current_function_;                       // 現在解析中の関数名 (呼び出しグラフ構築用)
    type_t current_return_type_;                         // 現在解析中の関数の戻り値型 (return文の整合性検査用)

    // 解析メソッド
    void index_global_decls();                              // 1パス目: グローバル宣言の索引作成と名前の重複検査
    void collect_globals();                                 // 2パス目: const変数・構造体定義・グローバル変数の登録と関数名の収集
    // 定数式をコンパイル時に計算する (初期化子・配列サイズ・case値)
    // 実行時の演算と同じく，整数昇格と符号の規則に従い32ビットで折り返した値を返す
    // sizeof(変数名)の解決にシンボルテーブル参照が必要なため非static
    const_value_t eval_const_expr(const node_t *expr);
    // 二項演算の定数式を計算する (両辺は計算済み)
    static const_value_t eval_const_binop(const node_t *expr, const const_value_t &l, const const_value_t &r);
    static long long const_symbol_value(const symbol_t *sym);   // const変数のシンボルが保持する値を型に応じて解釈して返す
    static int calc_array_words(const type_t &type);        // 配列が占有するワード数を計算する
    // 宣言ノードの型を確定させる (構造体型なら構造体定義を解決し，配列なら要素数を計算して畳み込む．確定済みなら何もしない)
    // 後方の宣言も解決できるよう，宣言順によらず必要になった時点で呼ばれる
    void resolve_decl_type(node_t *decl);
    void resolve_struct_def(const std::string &name);       // 構造体定義のメンバ構成を確定させて登録する (登録済みなら何もしない)
    // グローバルのconst変数を名前から解決してシンボルを返す (値が未確定なら計算して登録する．該当する宣言がなければnullptr)
    const symbol_t *resolve_global_const(const std::string &name);
    void begin_resolving(const node_t *decl);               // 宣言の解決を始める (解決中の宣言に再び到達したら循環参照としてエラー)
    void end_resolving(const node_t *decl);                 // 宣言の解決を終える
    // 構造体型の変数1つ分(配列宣言ならその配列全体分)のアドレスを確保し，シンボル(symbol_t)を生成して返す
    // decl: 構造体変数の宣言ノード(型・変数名・配列サイズ式を持つ)．location: グローバル/ローカルの区別
    // (シンボル表への格納自体は呼び出し元(collect_globals/analyze_local_decl)がグローバル/ローカルの
    //  区別に応じて行うため，この関数はシンボルの生成・アドレス確保だけに専念する．
    //  配列サイズを定数式から計算し，宣言ノードの子を計算済みの数値に置き換える(畳み込む)ため，
    //  declの中身を書き換える必要があり，読み取り専用にはできない)
    symbol_t *register_struct_var(node_t *decl, location_t location);
    // const変数の初期化子を定数式として計算し，値を持つシンボル(置き場所LOC_CONST)を生成して返す
    // (register_struct_varと同じく，シンボル表への格納はグローバル/ローカルの区別を知る呼び出し元が行う)
    symbol_t *register_const_var(const node_t *decl);
    // 型に構造体名が現れる場合(ポインタの指す先・関数ポインタの引数と戻り値を含む)，その構造体が定義済みであることを確かめる
    void check_type_exists(const type_t &type, const loc_t &loc) const;
    // ポインタ型のグローバル変数の初期化子が，コンパイル時に値が決まる番地の式であることを確かめる
    // (変数の値・ローカル変数の番地・関数呼び出しを使う初期化子はエラーにする)
    void check_global_pointer_inits();
    // コンパイル時に値が決まる番地の式(nullptr・グローバル変数や関数の番地と，それに整数定数を足し引きした式，
    // およびそれらや整数定数をポインタへキャストした式)かを返す
    static bool is_address_constant(const node_t *expr);
    // 番地がコンパイル時に決まる左辺値(グローバル変数と，その定数の添字の要素・メンバ)かを返す
    static bool is_static_lvalue(const node_t *expr);
    // 値がコンパイル時に決まる整数の式かを返す (名前解決・定数の埋め込みを終えた式を対象にする)
    static bool is_integer_constant(const node_t *expr);
    void analyze_functions();                               // 3パス目: 各関数本体を検査する
    void analyze_block(node_t *block);                      // ブロックを検査する (新しいスコープを積む)
    void analyze_stmt(node_t *stmt);                        // 文を検査する
    void analyze_switch(node_t *stmt);                      // switch文を検査する
    void analyze_local_decl(node_t *decl);                  // ローカル変数宣言を検査し登録する
    void analyze_expr(node_t *expr);                        // 式を検査し名前解決・型注釈する
    // 評価した結果の値(整数・ポインタ)を使う式(代入の右辺・引数・戻り値・演算のオペランド等)を検査する
    // (値を持たないvoid・構造体をエラーにし，配列は先頭要素へのポインタとして型を書き込む)
    void analyze_value(node_t *expr);
    // 式文，およびfor文の更新部に書いた式(副作用のために評価し，求まった値はどこにも使わない式)を検査する
    // (式全体がvoidであることは許し，構造体をエラーにし，配列は先頭要素へのポインタとして型を書き込む)
    void analyze_discarded(node_t *expr);
    // 書き込み先・番地の取得対象になる式(左辺値)を検査し名前解決・型注釈する (値を読む側の検査は行わない)
    void analyze_lvalue(node_t *expr);
    void analyze_call(node_t *expr);                        // 関数呼び出し(直接・関数ポインタ経由)を検査する
    void analyze_binop(node_t *expr);                       // 二項演算を検査し，結果の型を注釈する
    // 構造体のメンバを名前から探す (見つからなければexprの位置でエラー)
    const struct_member_t &find_member(const std::string &struct_name, const node_t *expr) const;
    type_t func_pointer_type(const std::string &name) const;  // その関数を指す関数ポインタの型を返す
    // 式srcの値を，型dstの格納先(代入・初期化する変数，関数の引数，戻り値)へ格納できるか検査する
    // 格納先の型と値の型のどちらかがポインタ(nullptrを含む)の場合に，ポインタと整数の取り違えと，
    // 指す先の型の違いをエラーにする(整数どうしは型が異なっても格納できる)．contextはエラーメッセージ用の格納の種類
    static void check_assignable(const type_t &dst, const node_t *src, const std::string &context);
    static bool is_same_type(const type_t &a, const type_t &b);   // 2つの型が(ポインタの指す先を含め)同じかを返す
    // print/streq/strcopyに共通する引数検査を行う (builtin_nameはエラーメッセージ用の関数名)
    void check_char_array_operand(node_t *target, const std::string &builtin_name);
    // 演算の対象がスカラー(整数またはポインタ)であることを検査する (operationはエラーメッセージ用の演算名)
    static void check_scalar_operand(const node_t *target, const std::string &operation);
    const symbol_t *lookup_symbol(const std::string &name) const;  // 名前からシンボルを探す (スコープ→グローバル)
    // 変数1つ分の領域を確保し，その先頭のオフセット(ローカル)または絶対番地(グローバル)を返す
    // (グローバルは0番地から上へ，ローカルは関数ごとにフレーム内のローカル変数領域の先頭から確保する)
    int alloc_var(int bytes, location_t location);
};
