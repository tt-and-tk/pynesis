#pragma once
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "analyzer.hpp"

// ハードウェア制約: 演算結果を格納するRAXレジスタのアセンブリ表記 (6'h1e)
const std::string RAX_REGISTER = "r30";

// 注釈付きASTとシンボルテーブルを受け取り，アセンブリコードを生成するジェネレータ
class Generator {
public:
    Generator(node_t *root, const std::map<std::string, const symbol_t *> &symbols,
              const std::map<std::string, std::vector<const symbol_t *>> &func_params,
              const std::map<std::string, struct_def_t> &struct_defs,
              int scratch_base, std::ofstream &asm_file);
    void operator()();   // コード生成を実行して .asm に書き出す

private:
    node_t *root_;                                            // 注釈付きAST
    const std::map<std::string, const symbol_t *> &symbols_;  // シンボルテーブル (変数名→番地)
    const std::map<std::string, std::vector<const symbol_t *>> &func_params_;  // 関数名→パラメータのシンボル列
    const std::map<std::string, struct_def_t> &struct_defs_;  // 構造体名→メンバ構成 (構造体配列の要素間隔計算に使う)
    // レジスタ退避領域の先頭番地．r{reg}を呼び出しをまたいで保持したいとき，scratch_base_ + reg*4番地へ退避する
    const int scratch_base_;
    std::ofstream &asm_file_;                                 // 出力先アセンブリファイル
    int label_count_ = 0;                                     // 局所ラベルの連番カウンタ (.L0, .L1, ...)
    // break/continueの飛び先ラベルのスタック (最内が末尾)
    // continueはループのみ，breakはループとswitchの両方が積む
    std::vector<std::string> break_labels_;
    std::vector<std::string> continue_labels_;

    // 生成メソッド (gen_で始まる)
    void gen_program();              // プログラム全体 (.global宣言 + main優先で各関数を出力)
    void gen_global_inits();         // グローバル変数の初期化 (mainの先頭に出力)
    void gen_func(node_t *func);     // 関数定義 (ラベル + 本体)
    void gen_block(node_t *block);   // ブロック (中の文を順に生成)
    void gen_stmt(node_t *stmt);     // 文 (種別ごとに振り分け)
    void gen_var_decl(node_t *decl); // 変数宣言 (初期化子があれば代入コードを生成)
    void gen_if(node_t *stmt);       // if文 (条件分岐)
    void gen_while(node_t *stmt);    // while文 (ループ)
    void gen_for(node_t *stmt);      // for文 (ループ)
    void gen_do_while(node_t *stmt); // do-while文 (末尾判定ループ)
    void gen_switch(node_t *stmt);   // switch文 (多分岐)
    void gen_expr(node_t *expr, int reg);  // 式を評価し結果をr{reg}に残す (レジスタスタック方式)
    // 式を評価し結果を指定レジスタに残す．評価前後で，別に指定したレジスタの値をメモリへ退避・復元する
    void gen_expr_protecting(node_t *expr, int reg, int protect_reg);
    void gen_binop_instr(const std::string &op, int dst, int lhs, int rhs);  // r{dst}=r{lhs} op r{rhs}を出力
    void gen_load(int reg, const symbol_t *sym);   // 変数をr{reg}へ読み込む (レジスタ直結ならmov・メモリならrm)
    void gen_store(int reg, const symbol_t *sym);  // r{reg}を変数へ書き込む (レジスタ直結ならmov・メモリならwm)
    void gen_sign_extend(int reg, int bits);       // r{reg}の下位bitsビットを符号として32ビットに符号拡張する (char/shortロード後に使用)
    void gen_array_load(node_t *expr, int reg);    // 配列要素をr{reg}へ読み込む
    void gen_array_store(node_t *expr, int val_reg, int work_reg);  // r{val_reg}を配列要素へ書き込む
    void gen_array_base_addr(int reg, const symbol_t *sym);  // 配列の先頭アドレスをr{reg}に載せる (直接配列は即値，配列パラメータは間接読み出し)
    // 構造体配列要素のメンバ(arr[i].member)の実アドレスをr{reg}に計算する．
    // アドレス = 配列先頭番地 + メンバオフセット(コンパイル時定数) + インデックス(実行時)×構造体1要素分のバイト数．
    // protect_regを指定すると，インデックス式の評価中もそのレジスタの値を保護する(既に確定した値を持つとき使う)
    void gen_struct_array_member_addr(node_t *member_access, int reg, int protect_reg = -1);
    // 構造体メンバ配列アクセス(children.size()==2のND_ARRAY_ACCESS)の配列先頭アドレスをr{addr_reg}に載せる．
    // 通常の単一構造体変数のメンバ配列はコンパイル時アドレス確定(gen_array_base_addr)，
    // 構造体配列要素のメンバ配列は実行時アドレス計算(gen_struct_array_member_addr)に振り分ける
    void gen_member_array_base(node_t *expr, int addr_reg, int protect_reg = -1);
    // r{reg}が指すメモリ番地から，型に応じたマスクでr{reg}へ読み込む(レジスタ間接アドレッシング)．
    // 構造体配列要素のメンバ等，実行時に計算したアドレスからスカラー値を読むときに使う
    void gen_load_indirect(int reg, const type_t &type);
    // r{val_reg}の値を，r{addr_reg}が指すメモリ番地へ型に応じたマスクで書き込む(レジスタ間接アドレッシング)
    void gen_store_indirect(int addr_reg, int val_reg, const type_t &type);
    void gen_string_init(int base_addr, const std::string &str);  // 文字列をchar配列に書き込む初期化コードを生成する
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
};
