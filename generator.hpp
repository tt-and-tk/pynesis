#pragma once
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "analyzer.hpp"

// ハードウェア制約: 演算結果を格納するRAXレジスタのアセンブリ表記 (6'h1e)
const std::string RAX_REGISTER = "r30";
// ハードウェア制約: スタックポインタ(SP)のアセンブリ表記 (6'h10)
const std::string SP_REGISTER = "r16";

// 注釈付きASTとシンボルテーブルを受け取り，アセンブリコードを生成するジェネレータ
//
// ローカル変数・パラメータ・レジスタの退避先は，呼び出しごとにスタック上へ確保するフレームに置く．
// フレームは関数の先頭でSPを下げて確保し，復帰の直前に戻す．プロローグ直後のSPを起点とした
// レイアウトは次のとおりで，パラメータと戻り先は呼び出し元がSPより下へ書き込んだものが，
// SPを下げた結果としてフレームの一部になる．
//
//   SP+0                    : ローカル変数領域 (Lバイト)
//   SP+L                    : レジスタ退避領域 (Sバイト．r{i}の退避先はSP+L+i*4)
//   SP+L+S                  : パラメータ領域 (4*nバイト．呼び出し元が書き込む)
//   SP+L+S+4n (=SP+F)       : 戻り先アドレス (CALLが積む)
class Generator {
public:
    Generator(node_t *root, const std::map<std::string, const symbol_t *> &symbols,
              const std::map<std::string, std::vector<const symbol_t *>> &func_params,
              const std::map<std::string, struct_def_t> &struct_defs,
              const std::map<std::string, int> &func_local_sizes,
              const std::map<std::string, std::set<std::string>> &call_graph,
              int global_size, std::ofstream &asm_file);
    void operator()();   // コード生成を実行して .pt に書き出す

private:
    node_t *root_;                                            // 注釈付きAST
    const std::map<std::string, const symbol_t *> &symbols_;  // シンボルテーブル (変数名→番地)
    const std::map<std::string, std::vector<const symbol_t *>> &func_params_;  // 関数名→パラメータのシンボル列
    const std::map<std::string, struct_def_t> &struct_defs_;  // 構造体名→メンバ構成 (構造体配列の要素間隔計算に使う)
    const std::map<std::string, int> &func_local_sizes_;      // 関数名→ローカル変数領域のバイト数
    const std::map<std::string, std::set<std::string>> &call_graph_;  // 関数名→直接呼び出す関数名の集合
    const int global_size_;                                   // グローバル変数・文字列リテラルが占めるバイト数
    std::ofstream &asm_file_;                                 // 出力先アセンブリファイル
    std::ostream *out_;                                       // 現在の出力先 (下見の間は捨てる先へ向ける)
    int local_size_ = 0;       // 生成中の関数のローカル変数領域のバイト数 (L)
    int spill_size_ = 0;       // 生成中の関数のレジスタ退避領域のバイト数 (S)
    int param_size_ = 0;       // 生成中の関数のパラメータ領域のバイト数 (4n)
    int max_spill_reg_ = -1;   // 生成中の関数が退避する最大のレジスタ番号 (下見で数え，退避領域の大きさを決める)
    std::map<std::string, int> func_frame_sizes_;             // 関数名→フレームのバイト数 (F．スタック使用量の検査に使う)
    std::map<std::string, int> stack_bytes_;                  // 関数名→その関数を呼び出してから戻るまでのスタック使用量
    int label_count_ = 0;                                     // 局所ラベルの連番カウンタ (.L0, .L1, ...)
    // break/continueの飛び先ラベルのスタック (最内が末尾)
    // continueはループのみ，breakはループとswitchの両方が積む
    std::vector<std::string> break_labels_;
    std::vector<std::string> continue_labels_;

    // 生成メソッド (gen_で始まる)
    void gen_program();              // プログラム全体 (.global宣言 + main優先で各関数を出力)
    void gen_global_inits();         // グローバル変数の初期化 (mainの先頭に出力)
    void gen_func(node_t *func);     // 関数定義 (ラベル + フレームの確保 + 本体)
    void gen_func_body(node_t *func);  // 関数の本体ブロックと末尾の復帰
    void gen_frame_alloc(bool is_release);  // フレームぶんSPを下げる/戻す命令 (フレームが空なら何も出力しない)
    void gen_block(node_t *block);   // ブロック (中の文を順に生成)
    void gen_stmt(node_t *stmt);     // 文 (種別ごとに振り分け)
    void gen_var_decl(node_t *decl); // 変数宣言 (初期化子があれば代入コードを生成)
    void gen_if(node_t *stmt);       // if文 (条件分岐)
    void gen_while(node_t *stmt);    // while文 (ループ)
    void gen_for(node_t *stmt);      // for文 (ループ)
    void gen_do_while(node_t *stmt); // do-while文 (末尾判定ループ)
    void gen_switch(node_t *stmt);   // switch文 (多分岐)
    void gen_expr(node_t *expr, int reg);  // 式を評価し結果をr{reg}に残す (レジスタスタック方式)
    // 式を評価し結果を指定レジスタに残す．評価前後で，別に指定したレジスタ(複数可)の値をメモリへ退避・復元する
    void gen_expr_protecting(node_t *expr, int reg, const std::vector<int> &protect_regs);
    // r{dst}=r{lhs} op r{rhs}を出力 (is_signedは符号付きで演算するか)
    void gen_binop_instr(const std::string &op, bool is_signed, int dst, int lhs, int rhs);
    // 変数をr{reg}へ読み込む (レジスタ直結ならmov・グローバルならrm・フレーム上ならrmr．
    // エラーは読み出す式の位置で報告する)
    void gen_load(int reg, const symbol_t *sym, const loc_t &loc);
    // r{reg}を変数へ書き込む (レジスタ直結ならmov・グローバルならwm・フレーム上ならwmr)
    void gen_store(int reg, const symbol_t *sym);
    // r{reg}の下位bitsビットを符号として32ビットに符号拡張する (符号付きchar/shortロード後に使用．r{work_reg}を作業用に使う)
    // 作業用レジスタが足りないエラーは，読み出す式の位置で報告する
    void gen_sign_extend(int reg, int bits, int work_reg, const loc_t &loc);
    // 配列要素の実アドレスをr{reg}に計算する．保護するレジスタを指定すると，添字の評価中もそれらの値を保護する
    void gen_array_elem_addr(node_t *expr, int reg, const std::vector<int> &protect_regs = {});
    // 番地が実行時に決まる代入先(配列要素または構造体配列要素のメンバ)の実アドレスをr{reg}に計算する
    void gen_runtime_addr(node_t *target, int reg, const std::vector<int> &protect_regs = {});
    // 配列の先頭アドレスをr{reg}に載せる
    // (グローバルの配列は即値，フレーム上の配列はSPにフレーム内オフセットを足した値，
    //  配列パラメータは呼び出し元が書き込んだ先頭アドレスの間接読み出し)
    void gen_array_base_addr(int reg, const symbol_t *sym);
    // 構造体配列要素のメンバ(arr[i].member)の実アドレスをr{reg}に計算する．
    // アドレス = 配列先頭番地 + メンバオフセット(コンパイル時定数) + インデックス(実行時)×構造体1要素分のバイト数．
    // 保護するレジスタを指定すると，インデックス式の評価中もそれらの値を保護する(既に確定した値を持つとき使う)
    void gen_struct_array_member_addr(node_t *member_access, int reg, const std::vector<int> &protect_regs = {});
    // 構造体メンバ配列アクセス(children.size()==2のND_ARRAY_ACCESS)の配列先頭アドレスをr{addr_reg}に載せる．
    // 通常の単一構造体変数のメンバ配列はコンパイル時アドレス確定(gen_array_base_addr)，
    // 構造体配列要素のメンバ配列は実行時アドレス計算(gen_struct_array_member_addr)に振り分ける
    void gen_member_array_base(node_t *expr, int addr_reg, const std::vector<int> &protect_regs = {});
    // r{reg}が指すメモリ番地から，型に応じたマスクでr{reg}へ読み込む(レジスタ間接アドレッシング)．
    // 構造体配列要素のメンバ等，実行時に計算したアドレスからスカラー値を読むときに使う．
    // 符号付きchar/shortの符号拡張ではr{work_reg}を作業用に使い，エラーは読み出す式の位置で報告する
    void gen_load_indirect(int reg, const type_t &type, int work_reg, const loc_t &loc);
    // r{val_reg}の値を，r{addr_reg}が指すメモリ番地へ型に応じたマスクで書き込む(レジスタ間接アドレッシング)
    void gen_store_indirect(int addr_reg, int val_reg, const type_t &type);
    // 文字列をchar配列に書き込む初期化コードを生成する (作業用にr0を使う)
    void gen_string_init(const symbol_t *sym, const std::string &str);
    void gen_print_string(const symbol_t *sym, int reg);  // char配列をヌル終端まで1文字ずつ出力するループを生成する
    void gen_scan_line(const symbol_t *sym, int reg);     // 標準入力を改行まで読み込みchar配列へヌル終端付きで格納するループを生成する
    void gen_streq(const symbol_t *sym_a, const symbol_t *sym_b, int reg);  // 2つのchar配列の内容を比較しr{reg}へ0/1を格納する
    void gen_strcopy(const symbol_t *dst, const symbol_t *src, int reg);   // char配列srcの内容をヌル終端付きでdstへコピーする
    void gen_compare(node_t *expr, int reg);   // 比較演算を0/1の値としてr{reg}に生成
    void gen_logical(node_t *expr, int reg);   // 論理 && / || を短絡評価しr{reg}に0/1を生成
    void gen_ternary(node_t *expr, int reg);   // 三項演算子 ?: の結果をr{reg}に生成
    void gen_incdec(node_t *expr, int reg, bool is_prefix);  // ++/-- (前置は新値・後置は旧値をr{reg}に残す)
    void gen_unary(node_t *expr, int reg);     // 単項 -/+/~/! の結果をr{reg}に生成
    // condが偽/真ならlabelへ分岐 (評価にr{reg}・r{reg+1}を使う．制御構文からはreg=0で呼ぶ)
    void gen_branch_if_false(node_t *cond, const std::string &label, int reg = 0);
    void gen_branch_if_true(node_t *cond, const std::string &label, int reg = 0);
    std::string new_label();         // 一意な局所ラベル (.Ln) を生成する

    // 補助メソッド (gen_ 本体からは独立した，AST走査などの下請け処理)
    // AST全体(全関数の本体)を再帰的に走査し，式中に現れる文字列リテラル(匿名グローバル配列)を集める
    // 変数宣言の初期化子として使われた文字列リテラルはsymを持たないため対象外
    void collect_string_literals(node_t *node, std::vector<node_t *> &out);
    // フレーム上の変数(ローカル変数・パラメータ)の，プロローグ直後のSPから数えたオフセットを返す
    int frame_offset(const symbol_t *sym) const;
    int spill_offset(int reg) const;   // r{reg}の退避枠の，プロローグ直後のSPから数えたオフセットを返す
    // 引数を書き込む位置の，呼び出し元の現在のSPから数えたオフセットを返す
    // (arg_count個の引数のindex番目．呼び出し先がフレームを確保するとパラメータ領域になる位置)
    static int arg_offset(int arg_count, int index);
    // 関数本体を出力を捨てて一度生成し，退避に使う最大のレジスタ番号を数えて退避領域の大きさを決める
    // (退避するレジスタはフレームの大きさに依存しないため，仮の大きさで生成しても結果は変わらない)
    int measure_spill_size(node_t *func);
    // グローバル変数とスタックがメモリ容量に収まるか検査する
    // (スタック使用量はmainを起点に呼び出しグラフを辿って求める．再帰があると深さが実行時にしか
    //  決まらないため検査しない)
    void check_memory_usage();
    // funcを呼び出してから戻るまでに使うスタックのバイト数(最大)を返す．
    // path: 現在の探索経路(再帰の検出用)．再帰を見つけた場合はis_recursiveをtrueにする
    // (どの経路から到達しても使用量は同じになるため，一度求めた値は記録して使い回す)
    int stack_bytes_dfs(const std::string &func, std::set<std::string> &path, bool &is_recursive);
};
