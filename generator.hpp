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
// ヌルポインタの値 (アドレスバス幅を超える存在しえない番地のため，参照するとCPUが必ず停止する)
const long long NULLPTR_VALUE = 0x80000000LL;

// 注釈付きASTと意味解析の結果を受け取り，アセンブリコードを生成するジェネレータ
class Generator {
public:
    Generator(node_t *root, const analysis_result_t &analysis, std::ofstream &asm_file);
    void operator()();   // コード生成を実行して .pt に書き出す

private:
    node_t *root_;                                            // 注釈付きAST
    const analysis_result_t analysis_;                        // 意味解析の結果
    std::ofstream &asm_file_;                                 // 出力先アセンブリファイル
    std::ostream *out_;                                       // 現在の出力先 (数えるためだけの生成では捨てる先を指す)
    int local_size_ = 0;       // 生成中の関数のローカル変数領域のバイト数
    int spill_size_ = 0;       // 生成中の関数のレジスタ退避領域のバイト数
    int param_size_ = 0;       // 生成中の関数の引数領域のバイト数
    int max_spill_reg_ = -1;   // 生成中の関数が退避する最大のレジスタ番号
    std::map<std::string, int> func_frame_sizes_;             // 関数名→フレームのバイト数
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
    void gen_frame_enter();          // フレームを確保する命令 (関数の先頭に置く)
    void gen_frame_leave();          // フレームを解放する命令 (復帰の直前に置く)
    void gen_sp_shift(const std::string &mnemonic);  // フレームの大きさだけSPを動かす命令 (確保・解放で共用する)
    void gen_block(node_t *block);   // ブロック (中の文を順に生成)
    void gen_stmt(node_t *stmt);     // 文 (種別ごとに振り分け)
    void gen_var_decl(node_t *decl); // 変数宣言 (初期化子があれば代入コードを生成)
    void gen_if(node_t *stmt);       // if文 (条件分岐)
    void gen_while(node_t *stmt);    // while文 (ループ)
    void gen_for(node_t *stmt);      // for文 (ループ)
    void gen_do_while(node_t *stmt); // do-while文 (末尾判定ループ)
    void gen_switch(node_t *stmt);   // switch文 (多分岐)
    void gen_expr(node_t *expr, int reg);  // 式を評価し結果をr{reg}に残す (レジスタスタック方式)
    void gen_call(node_t *expr, int reg);  // 関数呼び出しを生成し，戻り値をr{reg}に残す
    // ポインタに整数を足し引きする演算・ポインタどうしの差の結果をr{reg}に求める
    // 呼び出す前に，左辺をr{reg}・右辺をr{reg+1}へ評価しておくこと
    void gen_pointer_arith(node_t *expr, int reg);
    // r{reg}の値(添字・ポインタに足し引きする整数)にbytesを掛け，番地のずれのバイト数に換算する
    // (r{work_reg}を作業用に使う．エラーは式exprの位置で報告する)
    void gen_scale(int reg, int work_reg, long long bytes, const node_t *expr);
    // r{reg}の値を，arg_count個の引数のindex番目を渡す位置へ書き込む
    void gen_arg_store(int reg, const type_t &type, int arg_count, int index);
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
    // 左辺値(変数・配列要素・構造体メンバ・間接参照)が置かれているメモリ番地をr{reg}に求める．
    // 呼び出し元が値を持っているレジスタをprotect_regsに指定すると，添字やポインタの式の評価で関数を呼ぶ場合に，
    // その前後でそれらをメモリへ退避・復元して値を保つ
    void gen_lvalue_addr(node_t *target, int reg, const std::vector<int> &protect_regs = {});
    // 配列要素の実アドレスをr{reg}に計算する．保護するレジスタを指定すると，添字の評価中もそれらの値を保護する
    void gen_array_elem_addr(node_t *expr, int reg, const std::vector<int> &protect_regs = {});
    // 変数の番地をr{reg}に載せる (グローバル変数は即値，フレーム上の変数はSPにフレーム内オフセットを足した値)
    void gen_var_addr(int reg, const symbol_t *sym);
    // 構造体配列要素のメンバ(arr[i].member，構造体ポインタの添字p[i].memberを含む)の実アドレスをr{reg}に計算する．
    // アドレス = 先頭番地 + メンバオフセット(コンパイル時定数) + インデックス(実行時)×構造体1要素分のバイト数．
    // 先頭番地は，構造体の配列なら配列の番地，構造体へのポインタなら指している番地．
    // 保護するレジスタを指定すると，インデックス式の評価中もそれらの値を保護する(既に確定した値を持つとき使う)
    void gen_struct_array_member_addr(node_t *member_access, int reg, const std::vector<int> &protect_regs = {});
    // 基底の構造体の番地を実行時に求め，メンバのオフセットを足してメンバの実アドレスをr{reg}に計算する
    // (構造体ポインタの指す先のメンバ(p->member)・基底の式に添字を付けた要素のメンバ(s.items[i].member等))
    // 呼び出し元が値を持っているレジスタを指定すると，基底の式の評価で関数を呼ぶ場合に，その前後で退避・復元して値を保つ
    void gen_offset_member_addr(node_t *member_access, int reg, const std::vector<int> &protect_regs = {});
    // r{reg}が指すメモリ番地から，型に応じたマスクでr{reg}へ読み込む(レジスタ間接アドレッシング)．
    // 構造体配列要素のメンバ・ポインタの指す先等，実行時に計算したアドレスからスカラー値を読むときに使う．
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
    // 指定したノード(関数定義・変数宣言)以下を再帰的に走査し，式中に現れる文字列リテラル(匿名グローバル配列)を集める
    // char配列の初期化子として使われた文字列リテラル(char msg[] = "hi")は配列自体へ書き込むためsymを持たず，対象外
    void collect_string_literals(node_t *node, std::vector<node_t *> &out);
    // フレーム上の変数(ローカル変数・引数)の，フレームの基準から数えたオフセットを返す
    int calc_frame_offset(const symbol_t *sym) const;
    int calc_spill_offset(int reg) const;   // r{reg}の退避枠の，フレームの基準から数えたオフセットを返す
    int calc_frame_size() const;            // 生成中の関数のフレームのバイト数を返す
    // arg_count個の引数のindex番目を書き込む位置の，呼び出し元の現在のSPから数えたオフセットを返す
    static int calc_arg_offset(int arg_count, int index);
    int calc_spill_size(node_t *func);      // 関数が必要とするレジスタ退避領域のバイト数を求める
    void check_memory_usage();   // グローバル変数とスタックがメモリ容量に収まるか検査する
    // funcを呼び出してから戻るまでに使うスタックのバイト数(最大)を返す
    // path: 現在の探索経路(再帰の検出用)．recorded: 関数ごとに求めた使用量(再訪時に使い回す)
    // 再帰を見つけた場合はis_recursiveをtrueにする
    int calc_stack_bytes(const std::string &func, std::set<std::string> &path,
                         std::map<std::string, int> &recorded, bool &is_recursive);
};
