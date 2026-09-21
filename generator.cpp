#include <algorithm>
#include <iomanip>
#include <sstream>

#include "generator.hpp"

// 即値をアセンブリの表記にする
// 0x80000000以上の値は，10進で書くとアセンブラが出力するVerilogの10進定数(32ビット符号付き)の範囲を超えるため，16進(末尾h)で書く
static std::string imm_literal(long long value) {
    // intの範囲に収まる値(負の値を含む)は10進で書く
    if (value <= 0x7FFFFFFFLL) return std::to_string(value);
    std::ostringstream ss;   // 16進表記を組み立てるバッファ
    ss << std::uppercase << std::hex << (value & 0xFFFFFFFFLL) << 'h';
    return ss.str();
}

// 二項演算子の文字列を対応するアセンブリ命令に変換する (is_signedは符号付きで演算するか)
static std::string binop_mnemonic(const std::string &op, bool is_signed) {
    if (op == "+") return "add";
    if (op == "-") return "sub";
    if (op == "*") return "mul";
    if (op == "&") return "and";
    if (op == "|") return "or";
    if (op == "^") return "xor";
    if (op == "/") return is_signed ? "div" : "divu";     // 商 (余りは捨てる)
    if (op == "<<") return "sll";                          // 左シフト (空いたビットは符号によらず0で埋まる)
    if (op == ">>") return is_signed ? "sra" : "srl";      // 右シフト: 符号付きは算術シフト，符号なしは論理シフト
    // % は div/divu の4引数形式で別途生成する．比較・論理演算子は分岐の段階で対応する
    throw std::string("compiler error: unsupported binary operator '") + op + "'";
}

// 比較演算子かどうかを返す
static bool is_comparison(const std::string &op) {
    return op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=";
}

// 比較演算子の「否定」に対応するF系命令を返す (偽のとき分岐させるのに使う．is_signedは符号付きで比較するか)
// 大小比較の符号なし版は末尾にuを付けた命令になる (一致判定は符号によらないため同じ命令)
static std::string negated_branch(const std::string &op, bool is_signed) {
    const std::string u = is_signed ? "" : "u";     // 符号なしの大小比較に付ける接尾辞
    if (op == "==") return "ne";        // ==の否定は!=
    if (op == "!=") return "eq";        // !=の否定は==
    if (op == "<")  return "egt" + u;   // <の否定は>=
    if (op == ">")  return "elt" + u;   // >の否定は<=
    if (op == "<=") return "gt" + u;    // <=の否定は>
    if (op == ">=") return "lt" + u;    // >=の否定は<
    throw std::string("compiler error: not a comparison operator '") + op + "'";
}

// 比較演算子に「そのまま」対応するF系命令を返す (真のとき分岐させるのに使う．is_signedは符号付きで比較するか)
static std::string comparison_branch(const std::string &op, bool is_signed) {
    const std::string u = is_signed ? "" : "u";     // 符号なしの大小比較に付ける接尾辞
    if (op == "==") return "eq";
    if (op == "!=") return "ne";
    if (op == "<")  return "lt" + u;
    if (op == ">")  return "gt" + u;
    if (op == "<=") return "elt" + u;
    if (op == ">=") return "egt" + u;
    throw std::string("compiler error: not a comparison operator '") + op + "'";
}

// 比較・二項演算の式を符号付きで行うかを，両オペランドの型から返す
static bool is_signed_binop(const node_t *expr) {
    return is_signed_operation(expr->sval, expr->children[0]->type, expr->children[1]->type);
}

// 条件式として偽になる値の即値表記を返す (ポインタはnullptr，整数は0)
static std::string false_literal(const type_t &type) {
    return imm_literal(is_pointer_like(type) ? NULLPTR_VALUE : 0);
}

// 命令出力の慣例:
// mov/rm/wm の即値モードではrs1(第1レジスタ)が無視される．
// 以降のコードでrs1の位置に書く r0 は値を持たないダミーであり，r0の値は読まれも書かれもしない．
// (例外: switchのディスパッチでは r0 に条件値を入れて実際に使う)

// 式の中に関数呼び出しが含まれるかどうかを再帰的に調べる
static bool contains_call(const node_t *expr) {
    if (expr->kind == ND_CALL) return true;
    for (const node_t *child : expr->children) {
        if (contains_call(child)) return true;
    }
    return false;
}

// 代入先の番地が実行時に決まる(配列要素・ポインタの指す先・それらに属するメンバである)かどうかを返す
static bool has_runtime_addr(const node_t *target) {
    // 配列要素・間接参照そのもの，またはメンバの前に書いた式(基底)が構造体変数そのものでないメンバアクセス
    // (arr[i].m・p->m等)なら，基底の番地が実行時に決まるため実行時に番地が決まる
    return target->kind == ND_ARRAY_ACCESS || target->kind == ND_DEREF
        || (target->kind == ND_MEMBER_ACCESS && target->children[0]->kind != ND_VAR);
}

// コンストラクタ: AST・意味解析の結果・出力先を受け取る
Generator::Generator(node_t *root, const analysis_result_t &analysis, std::ofstream &asm_file)
    : root_(root), analysis_(analysis), asm_file_(asm_file), out_(&asm_file) {}

// コード生成を実行して .pt に書き出す
void Generator::operator()() {
    this->gen_program();
    // 全関数のフレームの大きさが確定したので，メモリ容量に収まるか検査する
    this->check_memory_usage();
}

// プログラム全体を生成する
// .global宣言で全関数名を列挙し，main関数を先頭に各関数を出力する
void Generator::gen_program() {
    // .global宣言: 子を走査して全関数名を集める (アセンブリは定義前に全関数の宣言が必要)
    bool first = true;
    (*this->out_) << ".global ";
    for (node_t *child : this->root_->children) {
        if (child->kind != ND_FUNC_DEF) continue;   // 関数定義のみ対象 (グローバル変数は除く)
        if (!first) (*this->out_) << ", ";
        (*this->out_) << child->sval;
        first = false;
    }
    (*this->out_) << "\n";

    // main関数を先頭に出力する (アセンブリはmainを一番最初に書く必要がある)
    for (node_t *child : this->root_->children) {
        if (child->kind == ND_FUNC_DEF && child->sval == "main") {
            (*this->out_) << "\n";
            this->gen_func(child);
            break;
        }
    }

    // main以外の関数を出力する
    for (node_t *child : this->root_->children) {
        if (child->kind == ND_FUNC_DEF && child->sval != "main") {
            (*this->out_) << "\n";
            this->gen_func(child);
        }
    }
}

// グローバル変数の初期化コードを生成する
// プログラム直下の変数宣言を走査し，初期化子があるものの初期化命令を出力する
// (mainが最初に実行されるため，mainの先頭で呼び出す)
void Generator::gen_global_inits() {
    for (node_t *child : this->root_->children) {
        if (child->kind == ND_VAR_DECL) {
            this->gen_var_decl(child);
        }
    }

    // 式中に現れる文字列リテラル(匿名グローバル配列)も，ここで1回だけ初期化する
    // (呼び出し回数に関わらず値が変わらない定数データのため，通常のグローバル変数と同じ扱い．
    //  関数本体とグローバル変数の初期化子のどちらにも現れうるため，両方から集める)
    std::vector<node_t *> string_lits;
    for (node_t *child : this->root_->children) {
        if (child->kind == ND_FUNC_DEF || child->kind == ND_VAR_DECL) {
            this->collect_string_literals(child, string_lits);
        }
    }
    for (node_t *lit : string_lits) {
        this->gen_string_init(lit->sym, lit->sval);
    }
}

// 指定したノード(関数定義・変数宣言)以下を再帰的に走査し，式中に現れる文字列リテラル(匿名グローバル配列)を集める
// char配列の初期化子として使われた文字列リテラル(char msg[] = "hi")は配列自体へ書き込むためsymを持たず，対象外
void Generator::collect_string_literals(node_t *node, std::vector<node_t *> &out) {
    if (node == nullptr) return;
    if (node->kind == ND_STRING_LIT && node->sym != nullptr) {
        out.push_back(node);
    }
    for (node_t *child : node->children) {
        this->collect_string_literals(child, out);
    }
}

// 関数定義を生成する
void Generator::gen_func(node_t *func) {
    // この関数のフレームの各領域の大きさを求める
    this->local_size_ = this->analysis_.func_local_sizes.at(func->sval);
    this->param_size_ = static_cast<int>(this->analysis_.func_params.at(func->sval).size()) * 4;
    this->spill_size_ = this->calc_spill_size(func);
    this->func_frame_sizes_[func->sval] = this->calc_frame_size();

    // 関数ラベルを出力し，フレームを確保してから本体を生成する
    (*this->out_) << func->sval << ":\n";
    this->max_spill_reg_ = -1;
    this->gen_frame_enter();
    this->gen_func_body(func);

    // 本番の生成で退避したレジスタが，確保した退避領域に収まっていたことを確かめる
    // (式の形だけで退避するレジスタが決まる以上ここは通らないが，その前提が崩れた場合に
    //  フレーム上の他の値を壊した出力が黙って出ることを防ぐ)
    if ((this->max_spill_reg_ + 1) * 4 > this->spill_size_) {
        throw std::string("compiler error: register spill area is too small in '") + func->sval + "'";
    }
}

// 生成中の関数のフレームのバイト数を返す
int Generator::calc_frame_size() const {
    return this->local_size_ + this->spill_size_ + this->param_size_;
}

// 関数が必要とするレジスタ退避領域のバイト数を求める
// 退避するレジスタは本体を生成してみないと分からないため，出力を捨てて本体を一度生成し，
// 退避に使った最大のレジスタ番号から求める
// (退避するかどうかは式の形だけで決まりフレームの大きさに左右されないため，
//  大きさが未確定のまま生成しても，数えた結果は本番の生成でもそのまま通用する)
int Generator::calc_spill_size(node_t *func) {
    const int label_count_before = this->label_count_;   // 生成を始める前のラベルの連番
    const int spill_size_before = this->spill_size_;     // 生成を始める前の退避領域の大きさ
    std::ostringstream discarded;                        // 出力を捨てる先

    // 出力先を捨てる先へ向け，退避領域の大きさに最大値を置いて本体を生成する
    this->out_ = &discarded;
    this->max_spill_reg_ = -1;
    this->spill_size_ = MAX_REG * 4;
    this->gen_func_body(func);
    const int spill_size = (this->max_spill_reg_ + 1) * 4;   // 退避したレジスタの本数から求めた大きさ

    // 出力先と，生成に伴って変わった状態を元へ戻す
    this->out_ = &this->asm_file_;
    this->label_count_ = label_count_before;
    this->spill_size_ = spill_size_before;
    return spill_size;
}

// 関数の本体と，末尾から関数を抜ける場合の復帰を生成する
void Generator::gen_func_body(node_t *func) {
    // mainの先頭でグローバル変数を初期化する (mainが最初に実行されるため)
    if (func->sval == "main") {
        this->gen_global_inits();
    }
    this->gen_block(func->children.back());   // 本体ブロック(最後の子)の文を生成する
    // 関数末尾のret (全関数にretが1つ以上必要)．return文を通らずに終端へ達した場合の復帰でもある
    this->gen_frame_leave();
    (*this->out_) << "    ret\n";
}

// フレームを確保する命令を出力する
void Generator::gen_frame_enter() {
    this->gen_sp_shift("sub");
}

// フレームを解放する命令を出力する
void Generator::gen_frame_leave() {
    this->gen_sp_shift("add");
}

// フレームの大きさだけSPを動かす命令を出力する
void Generator::gen_sp_shift(const std::string &mnemonic) {
    const int frame_size = this->calc_frame_size();   // フレームのバイト数
    // 引数もローカル変数もレジスタの退避も持たない関数の場合 (動かす領域がないため，これ以上何もしない)
    if (frame_size == 0) return;

    // 即値を直接加減算する命令がないため，大きさをいったんr0に載せてから計算する
    // (r0を使えるのは，フレームを動かすのが関数の開始直後と復帰の直前に限られ，どちらもr0が値を保持していないため)

    (*this->out_) << "    mov fh r0 r0 " << frame_size << "\n";
    (*this->out_) << "    " << mnemonic << " " << SP_REGISTER << " r0 " << SP_REGISTER << "\n";
}

// ブロックを生成する
// 中の文を上から順に生成する
void Generator::gen_block(node_t *block) {
    for (node_t *stmt : block->children) {
        this->gen_stmt(stmt);
    }
}

// 文を生成する
// 文の種別ごとに対応する生成処理へ振り分ける
void Generator::gen_stmt(node_t *stmt) {
    switch (stmt->kind) {
        // 変数宣言
        case ND_VAR_DECL:
            this->gen_var_decl(stmt);
            break;
        // 入れ子のブロック
        case ND_BLOCK:
            this->gen_block(stmt);
            break;
        // 代入文・関数呼び出し文・入出力文・増減文 (式文): 副作用のため評価する．結果(r0)は捨てる
        case ND_ASSIGN:
        case ND_CALL:
        case ND_PRINT:
        case ND_SCAN:
        case ND_STREQ:
        case ND_STRCOPY:
        case ND_UNOP:
        case ND_POST_UNOP:
            this->gen_expr(stmt, 0);
            break;
        // if文
        case ND_IF:
            this->gen_if(stmt);
            break;
        // while文
        case ND_WHILE:
            this->gen_while(stmt);
            break;
        // for文
        case ND_FOR:
            this->gen_for(stmt);
            break;
        // do-while文
        case ND_DO_WHILE:
            this->gen_do_while(stmt);
            break;
        // switch文
        case ND_SWITCH:
            this->gen_switch(stmt);
            break;
        // break文: 最内のループ/switchの脱出先へ飛ぶ (アナライザが内側であることを保証済み)
        case ND_BREAK:
            (*this->out_) << "    jmp " << this->break_labels_.back() << "\n";
            break;
        // continue文: 最内ループの継続先へ飛ぶ
        case ND_CONTINUE:
            (*this->out_) << "    jmp " << this->continue_labels_.back() << "\n";
            break;
        // return文: 戻り値があればRAX(r30)に書き込み，フレームを解放してから復帰する
        case ND_RETURN:
            if (!stmt->children.empty()) {
                this->gen_expr(stmt->children[0], 0);                  // 戻り値の式 → r0
                (*this->out_) << "    mov fh r0 " << RAX_REGISTER << "\n";  // r0 → RAX
            }
            this->gen_frame_leave();
            (*this->out_) << "    ret\n";
            break;
        default:
            break;
    }
}

// 変数宣言を生成する
// 初期化子があれば，初期値を変数の番地へ書き込むコードを生成する
void Generator::gen_var_decl(node_t *decl) {
    // const変数はメモリを持たず，参照箇所へ値が埋め込み済みのため何も出力しない
    if (decl->type.is_const) return;

    // 配列宣言: 文字列リテラルによる初期化のみ対応 (サイズ指定のみの宣言はスキップ)
    if (decl->type.is_array) {
        if (!decl->children.empty() && decl->children[0]->kind == ND_STRING_LIT) {
            this->gen_string_init(decl->sym, decl->children[0]->sval);
        }
        return;
    }
    // 初期化子がなければ何も出力しない (番地は確保済み，未初期化ローカルは不定値)
    if (decl->children.empty()) return;

    // 初期化式をr0に評価し，変数へ書き込む
    this->gen_expr(decl->children[0], 0);        // r0 = 初期値
    this->gen_store(0, decl->sym);               // 変数 = r0
}

// 文字列をchar配列のメモリに書き込む初期化コードを生成する
// 4文字ずつ1ワードにパックして書き込む (末尾にヌル終端を含む)
// 作業用にr0を使う(変数宣言の単位で生成され，そこではどのレジスタも値を保持していない)
void Generator::gen_string_init(const symbol_t *sym, const std::string &str) {
    const bool is_global = sym->location == LOC_GLOBAL;   // 絶対番地で書き込めるか
    // 書き込み先の位置(グローバル変数は絶対番地，ローカル変数はフレーム内オフセット)
    const int base = is_global ? sym->address : this->calc_frame_offset(sym);

    // ヌル終端を含めた全バイト列を構築する
    std::string data = str;
    data += '\0';
    // 4バイトずつワードにパックして書き込む
    const int word_count = (static_cast<int>(data.size()) + 3) / 4;
    for (int w = 0; w < word_count; w++) {
        // 1ワード = 4バイトをリトルエンディアン的にパックする (byte0が最下位)
        unsigned int word = 0;
        for (int b = 0; b < 4; b++) {
            const int idx = w * 4 + b;
            if (idx < static_cast<int>(data.size())) {
                word |= (static_cast<unsigned char>(data[idx]) << (b * 8));
            }
        }
        // ワードをメモリに書き込む (wワード目は先頭から w*4 バイト目の位置)
        (*this->out_) << "    mov fh r0 r0 " << imm_literal(word) << "\n";
        // グローバル変数の場合
        if (is_global) {
            (*this->out_) << "    wm fh r0 r0 " << (base + w * 4) << "\n";
        }
        // ローカル変数の場合
        else {
            (*this->out_) << "    wmr fh " << SP_REGISTER << " r0 " << (base + w * 4) << "\n";
        }
    }
}

// char配列をヌル終端(0)まで1文字ずつ標準出力へ出力するループを生成する
// ヌル終端が見つからない不正な配列でも，配列の宣言サイズで安全に打ち切る
// レジスタ使用: r{reg}=インデックス, r{reg+1}=ベースアドレス(不変), r{reg+2}=アドレス→文字値(作業用),
//              r{reg+3}=配列サイズ(打ち切り境界，不変), r{reg+4}=0(ヌル終端比較用，不変), r{reg+5}=1(インデックス加算用，不変)
void Generator::gen_print_string(const symbol_t *sym, int reg) {
    if (reg + 5 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers)");
    }
    const std::string loop = this->new_label();
    const std::string end = this->new_label();

    // 必要な変数をレジスタに格納する
    this->gen_var_addr(reg + 1, sym);                                           // r{reg+1} = ベースアドレス
    (*this->out_) << "    mov fh r0 r" << reg << " 0\n";                        // r{reg}   = インデックス(0)
    (*this->out_) << "    mov fh r0 r" << (reg + 3) << " " << sym->type.array_size << "\n";  // r{reg+3} = 配列サイズ(打ち切り境界)
    (*this->out_) << "    mov fh r0 r" << (reg + 4) << " 0\n";                  // r{reg+4} = 0 (ヌル終端比較用)
    (*this->out_) << "    mov fh r0 r" << (reg + 5) << " 1\n";                  // r{reg+5} = 1 (インデックス加算用)

    (*this->out_) << loop << ":\n";
    // 配列サイズに達したら打ち切る (ヌル終端がなくても無限ループ・範囲外読み出しを防ぐ)
    (*this->out_) << "    egt r" << reg << " r" << (reg + 3) << " " << end << "\n";
    // r{reg+2} = mem[base + index] (1バイト)
    (*this->out_) << "    add r" << (reg + 1) << " r" << reg << " r" << (reg + 2) << "\n";
    (*this->out_) << "    rm 1h r" << (reg + 2) << " r" << (reg + 2) << "\n";
    // ヌル終端なら終了
    (*this->out_) << "    eq r" << (reg + 2) << " r" << (reg + 4) << " " << end << "\n";
    (*this->out_) << "    print r" << (reg + 2) << "\n";
    (*this->out_) << "    add r" << reg << " r" << (reg + 5) << " r" << reg << "\n";
    (*this->out_) << "    jmp " << loop << "\n";
    (*this->out_) << end << ":\n";
}

// 標準入力を改行(\n=10)まで読み込み，char配列へヌル終端付きで格納するループを生成する
// 先頭の改行はすべて読み飛ばす．配列サイズ-1文字を超えたら追加のscanを行わず打ち切る
// (超過分は次にscanを実行したときに読み込まれる．そのためのscanが1回多く消費されることはない)
// DEL(0x7F，バックスペース)を受け取った場合は，直前に格納した1文字分インデックスを戻す(先頭では何もしない)
// レジスタ使用: r{reg}=読み込んだ文字, r{reg+1}=インデックス, r{reg+2}=ベースアドレス(不変), r{reg+3}=アドレス(作業用),
//              r{reg+4}='\n'(不変), r{reg+5}=配列サイズ-1(格納できる最大文字数，不変), r{reg+6}=1(インデックス加算用，不変),
//              r{reg+7}=0(ヌル終端書き込み用兼インデックス0判定用，不変), r{reg+8}=DEL(0x7F，不変)
void Generator::gen_scan_line(const symbol_t *sym, int reg) {
    if (reg + 8 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers)");
    }
    const std::string skip_loop = this->new_label();
    const std::string skip_end = this->new_label();
    const std::string read_loop = this->new_label();
    const std::string del_branch = this->new_label();
    const std::string scan_next = this->new_label();
    const std::string read_end = this->new_label();

    // 必要な変数をレジスタに格納する
    this->gen_var_addr(reg + 2, sym);                                                 // r{reg+2} = ベースアドレス
    (*this->out_) << "    mov fh r0 r" << (reg + 4) << " 10\n";                       // r{reg+4} = '\n'
    (*this->out_) << "    mov fh r0 r" << (reg + 5) << " " << (sym->type.array_size - 1) << "\n";  // r{reg+5} = 配列サイズ-1
    (*this->out_) << "    mov fh r0 r" << (reg + 6) << " 1\n";                        // r{reg+6} = 1
    (*this->out_) << "    mov fh r0 r" << (reg + 7) << " 0\n";                        // r{reg+7} = 0
    (*this->out_) << "    mov fh r0 r" << (reg + 8) << " 7Fh\n";                      // r{reg+8} = DEL(0x7F)

    // 先頭の改行はすべて読み飛ばす
    (*this->out_) << "    scan r" << reg << "\n";
    (*this->out_) << skip_loop << ":\n";
    (*this->out_) << "    ne r" << reg << " r" << (reg + 4) << " " << skip_end << "\n";  // 改行以外ならスキップ終了
    (*this->out_) << "    scan r" << reg << "\n";
    (*this->out_) << "    jmp " << skip_loop << "\n";
    (*this->out_) << skip_end << ":\n";

    // 改行が来るまで1文字ずつ配列へ格納する
    (*this->out_) << "    mov fh r0 r" << (reg + 1) << " 0\n";                        // r{reg+1} = インデックス(0)
    (*this->out_) << read_loop << ":\n";
    (*this->out_) << "    eq r" << reg << " r" << (reg + 4) << " " << read_end << "\n";  // 改行なら終了
    (*this->out_) << "    eq r" << reg << " r" << (reg + 8) << " " << del_branch << "\n";  // DELならバックスペース処理へ
    (*this->out_) << "    add r" << (reg + 2) << " r" << (reg + 1) << " r" << (reg + 3) << "\n";  // アドレス = base+index
    (*this->out_) << "    wm 1h r" << (reg + 3) << " r" << reg << "\n";                // buf[index] = 文字
    (*this->out_) << "    add r" << (reg + 1) << " r" << (reg + 6) << " r" << (reg + 1) << "\n";  // index += 1
    // 配列サイズ上限に達したら，これ以上scanせずに打ち切る (残りは次回のscanで読む)
    (*this->out_) << "    egt r" << (reg + 1) << " r" << (reg + 5) << " " << read_end << "\n";
    (*this->out_) << "    jmp " << scan_next << "\n";
    // バックスペース: 先頭(インデックス0)なら取り消す文字が無いので何もしない
    (*this->out_) << del_branch << ":\n";
    (*this->out_) << "    eq r" << (reg + 1) << " r" << (reg + 7) << " " << scan_next << "\n";
    (*this->out_) << "    sub r" << (reg + 1) << " r" << (reg + 6) << " r" << (reg + 1) << "\n";  // index -= 1
    (*this->out_) << scan_next << ":\n";
    (*this->out_) << "    scan r" << reg << "\n";
    (*this->out_) << "    jmp " << read_loop << "\n";
    (*this->out_) << read_end << ":\n";

    // ヌル終端を書き込む
    (*this->out_) << "    add r" << (reg + 2) << " r" << (reg + 1) << " r" << (reg + 3) << "\n";  // アドレス = base+index
    (*this->out_) << "    wm 1h r" << (reg + 3) << " r" << (reg + 7) << "\n";           // buf[index] = 0
}

// char配列2つの内容を先頭から1文字ずつ比較し，一致すれば1，不一致なら0をr{reg}へ格納する
// どちらかの宣言サイズに達してもヌル終端が見つからない場合は，安全のため不一致として打ち切る
// レジスタ使用: r{reg}=結果, r{reg+1}=インデックス, r{reg+2}=ベースアドレスA(不変), r{reg+3}=ベースアドレスB(不変),
//              r{reg+4}=打ち切り境界(不変)，r{reg+5}=アドレス(作業用，A/B共用)，r{reg+6}=文字A(作業用)，
//              r{reg+7}=文字B(作業用)，r{reg+8}=1(インデックス加算用，不変)，r{reg+9}=0(ヌル終端比較用，不変)
void Generator::gen_streq(const symbol_t *sym_a, const symbol_t *sym_b, int reg) {
    if (reg + 9 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers)");
    }
    const std::string loop = this->new_label();
    const std::string mismatch = this->new_label();
    const std::string match = this->new_label();
    const std::string end = this->new_label();
    // 打ち切り境界は両配列の宣言サイズの小さい方(コンパイル時に確定するため1本のレジスタで表せる)
    const int size_limit = std::min(sym_a->type.array_size, sym_b->type.array_size);

    // 各レジスタの役割に名前を付ける(この関数内では役割の使い回しをしない)
    const int r_result = reg;          // 比較結果(1=一致，0=不一致)
    const int r_index = reg + 1;       // インデックス
    const int r_base_a = reg + 2;      // ベースアドレスA(不変)
    const int r_base_b = reg + 3;      // ベースアドレスB(不変)
    const int r_limit = reg + 4;       // 打ち切り境界(不変)
    const int r_addr = reg + 5;        // アドレス(作業用，A/B共用)
    const int r_char_a = reg + 6;      // 配列Aから読んだ文字(作業用)
    const int r_char_b = reg + 7;      // 配列Bから読んだ文字(作業用)
    const int r_one = reg + 8;         // 1(インデックス加算用，不変)
    const int r_zero = reg + 9;        // 0(ヌル終端比較用，不変)

    // 必要な変数をレジスタに格納する
    this->gen_var_addr(r_base_a, sym_a);                               // ベースアドレスAを取得する
    this->gen_var_addr(r_base_b, sym_b);                               // ベースアドレスBを取得する
    (*this->out_) << "    mov fh r0 r" << r_index << " 0\n";           // インデックスを0で初期化する
    (*this->out_) << "    mov fh r0 r" << r_limit << " " << size_limit << "\n";  // 打ち切り境界を設定する
    (*this->out_) << "    mov fh r0 r" << r_one << " 1\n";             // インデックス加算用に1を格納する
    (*this->out_) << "    mov fh r0 r" << r_zero << " 0\n";            // ヌル終端比較用に0を格納する

    (*this->out_) << loop << ":\n";
    // インデックスが打ち切り境界を超えた場合，ヌル終端が見つからないまま両配列の宣言サイズに達したとみなし，
    // mismatch(不一致確定)へジャンプする
    (*this->out_) << "    egt r" << r_index << " r" << r_limit << " " << mismatch << "\n";
    // 配列Aの現在インデックスの文字を読み込む
    (*this->out_) << "    add r" << r_base_a << " r" << r_index << " r" << r_addr << "\n";
    (*this->out_) << "    rm 1h r" << r_addr << " r" << r_char_a << "\n";
    // 配列Bの現在インデックスの文字を読み込む
    (*this->out_) << "    add r" << r_base_b << " r" << r_index << " r" << r_addr << "\n";
    (*this->out_) << "    rm 1h r" << r_addr << " r" << r_char_b << "\n";
    // 読み込んだ文字が異なる場合，不一致が確定したのでmismatchへジャンプする
    (*this->out_) << "    ne r" << r_char_a << " r" << r_char_b << " " << mismatch << "\n";
    // 両方ともヌル終端(文字コード0)であった場合，先頭からここまで全て一致したとみなし，match(一致確定)へジャンプする
    (*this->out_) << "    eq r" << r_char_a << " r" << r_zero << " " << match << "\n";
    // 次の文字を比較するため，インデックスを1つ進めてloopの先頭へ戻る
    (*this->out_) << "    add r" << r_index << " r" << r_one << " r" << r_index << "\n";
    (*this->out_) << "    jmp " << loop << "\n";
    (*this->out_) << match << ":\n";
    // 比較結果を「一致」として格納し，end(終了処理)へジャンプする
    (*this->out_) << "    mov fh r0 r" << r_result << " 1\n";
    (*this->out_) << "    jmp " << end << "\n";
    (*this->out_) << mismatch << ":\n";
    // 比較結果を「不一致」として格納する
    (*this->out_) << "    mov fh r0 r" << r_result << " 0\n";
    (*this->out_) << end << ":\n";
}

// char配列srcの先頭からヌル終端まで1文字ずつdstへコピーする
// dstの宣言サイズ-1文字を超える場合は，そこで打ち切ってヌル終端を書き込む(scanと同じ安全な打ち切り)
// レジスタ使用: r{reg}=コピー中の文字(作業用), r{reg+1}=インデックス, r{reg+2}=ベースアドレス(コピー先，不変),
//              r{reg+3}=ベースアドレス(コピー元，不変), r{reg+4}=アドレス(作業用), r{reg+5}=0(ヌル終端書き込み用，不変),
//              r{reg+6}=コピー先の配列サイズ-1(打ち切り境界，不変), r{reg+7}=1(インデックス加算用，不変)
void Generator::gen_strcopy(const symbol_t *dst, const symbol_t *src, int reg) {
    if (reg + 7 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers)");
    }
    const std::string loop = this->new_label();
    const std::string truncate = this->new_label();
    const std::string finish = this->new_label();
    const std::string end = this->new_label();

    // 各レジスタの役割に名前を付ける(この関数内では役割の使い回しをしない)
    const int r_char = reg;            // コピー中の文字(作業用)
    const int r_index = reg + 1;       // インデックス
    const int r_base_dst = reg + 2;    // ベースアドレス(コピー先，不変)
    const int r_base_src = reg + 3;    // ベースアドレス(コピー元，不変)
    const int r_addr = reg + 4;        // アドレス(作業用)
    const int r_zero = reg + 5;        // 0(ヌル終端書き込み用，不変)
    const int r_limit = reg + 6;       // コピー先の配列サイズ-1(打ち切り境界，不変)
    const int r_one = reg + 7;         // 1(インデックス加算用，不変)

    // 必要な変数をレジスタに格納する
    this->gen_var_addr(r_base_dst, dst);                               // ベースアドレス(コピー先)を取得する
    this->gen_var_addr(r_base_src, src);                               // ベースアドレス(コピー元)を取得する
    (*this->out_) << "    mov fh r0 r" << r_index << " 0\n";           // インデックスを0で初期化する
    (*this->out_) << "    mov fh r0 r" << r_zero << " 0\n";            // ヌル終端書き込み用に0を格納する
    (*this->out_) << "    mov fh r0 r" << r_limit << " " << (dst->type.array_size - 1) << "\n";  // 打ち切り境界を設定する
    (*this->out_) << "    mov fh r0 r" << r_one << " 1\n";             // インデックス加算用に1を格納する

    (*this->out_) << loop << ":\n";
    // インデックスがコピー先の宣言サイズ-1文字を超えた場合，これ以上格納する余地がないので
    // truncate(打ち切り処理)へジャンプする
    (*this->out_) << "    egt r" << r_index << " r" << r_limit << " " << truncate << "\n";
    // コピー元の現在インデックスの文字を読み込む
    (*this->out_) << "    add r" << r_base_src << " r" << r_index << " r" << r_addr << "\n";
    (*this->out_) << "    rm 1h r" << r_addr << " r" << r_char << "\n";
    // 読み込んだ文字がヌル終端(文字コード0)であった場合，コピーすべき文字は終わったのでfinish(終端処理)へジャンプする
    (*this->out_) << "    eq r" << r_char << " r" << r_zero << " " << finish << "\n";
    // 読み込んだ文字をコピー先の現在インデックスへ書き込む
    (*this->out_) << "    add r" << r_base_dst << " r" << r_index << " r" << r_addr << "\n";
    (*this->out_) << "    wm 1h r" << r_addr << " r" << r_char << "\n";
    // 次の文字をコピーするため，インデックスを1つ進めてloopの先頭へ戻る
    (*this->out_) << "    add r" << r_index << " r" << r_one << " r" << r_index << "\n";
    (*this->out_) << "    jmp " << loop << "\n";
    (*this->out_) << truncate << ":\n";
    // コピー先の末尾(宣言サイズ-1文字目)にヌル終端を書き込み，end(終了処理)へジャンプする
    (*this->out_) << "    add r" << r_base_dst << " r" << r_limit << " r" << r_addr << "\n";
    (*this->out_) << "    wm 1h r" << r_addr << " r" << r_zero << "\n";
    (*this->out_) << "    jmp " << end << "\n";
    (*this->out_) << finish << ":\n";
    // コピー先の現在インデックスにヌル終端を書き込む
    (*this->out_) << "    add r" << r_base_dst << " r" << r_index << " r" << r_addr << "\n";
    (*this->out_) << "    wm 1h r" << r_addr << " r" << r_zero << "\n";
    (*this->out_) << end << ":\n";
}

// 一意な局所ラベル (.L0, .L1, ...) を生成して返す
std::string Generator::new_label() {
    return ".L" + std::to_string(this->label_count_++);
}

// 条件式condが偽のとき，labelへ分岐する命令を出力する
// 評価にはr{reg}・r{reg+1}を使う (式の途中で呼ばれても下位レジスタを壊さないため)
void Generator::gen_branch_if_false(node_t *cond, const std::string &label, int reg) {
    // r{reg+1}を使うため，上限(r15)を超えないことを確認する
    if (reg + 1 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at ")
              + loc_to_string(cond->loc);
    }
    // 比較条件: 否定したF系で「偽のとき飛ぶ」を1命令で表現する
    if (cond->kind == ND_BINOP && is_comparison(cond->sval)) {
        this->gen_expr(cond->children[0], reg);       // 左 → r{reg}
        this->gen_expr_protecting(cond->children[1], reg + 1, {reg});   // 右 → r{reg+1}
        (*this->out_) << "    " << negated_branch(cond->sval, is_signed_binop(cond))
                        << " r" << reg << " r" << (reg + 1) << " " << label << "\n";
    }
    // 一般条件: 値を評価し，偽(整数は0，ポインタはnullptr)なら飛ぶ
    else {
        this->gen_expr(cond, reg);                                                                    // cond → r{reg}
        (*this->out_) << "    mov fh r0 r" << (reg + 1) << " " << false_literal(cond->type) << "\n";  // r{reg+1} = 偽の値
        (*this->out_) << "    eq r" << reg << " r" << (reg + 1) << " " << label << "\n";              // 偽なら飛ぶ
    }
}

// 条件式condが真のとき，labelへ分岐する命令を出力する
// 評価にはr{reg}・r{reg+1}を使う
void Generator::gen_branch_if_true(node_t *cond, const std::string &label, int reg) {
    // r{reg+1}を使うため，上限(r15)を超えないことを確認する
    if (reg + 1 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at ")
              + loc_to_string(cond->loc);
    }
    // 比較条件: そのままのF系で「真のとき飛ぶ」を1命令で表現する
    if (cond->kind == ND_BINOP && is_comparison(cond->sval)) {
        this->gen_expr(cond->children[0], reg);       // 左 → r{reg}
        this->gen_expr_protecting(cond->children[1], reg + 1, {reg});   // 右 → r{reg+1}
        (*this->out_) << "    " << comparison_branch(cond->sval, is_signed_binop(cond))
                        << " r" << reg << " r" << (reg + 1) << " " << label << "\n";
    }
    // 一般条件: 値を評価し，真(整数は0以外，ポインタはnullptr以外)なら飛ぶ
    else {
        this->gen_expr(cond, reg);                                                                    // cond → r{reg}
        (*this->out_) << "    mov fh r0 r" << (reg + 1) << " " << false_literal(cond->type) << "\n";  // r{reg+1} = 偽の値
        (*this->out_) << "    ne r" << reg << " r" << (reg + 1) << " " << label << "\n";              // 偽以外なら飛ぶ
    }
}

// 比較演算を0/1の値としてr{reg}に生成する
// 左をr{reg}・右をr{reg+1}に評価し，比較が真なら1・偽なら0をr{reg}に置く
void Generator::gen_compare(node_t *expr, int reg) {
    const std::string t = this->new_label();      // 真の場合の飛び先
    const std::string end = this->new_label();
    this->gen_expr(expr->children[0], reg);        // 左 → r{reg}
    this->gen_expr_protecting(expr->children[1], reg + 1, {reg});    // 右 → r{reg+1}
    // 比較が真なら .Lt へ
    (*this->out_) << "    " << comparison_branch(expr->sval, is_signed_binop(expr))
                    << " r" << reg << " r" << (reg + 1) << " " << t << "\n";
    (*this->out_) << "    mov fh r0 r" << reg << " 0\n";   // 偽: r{reg} = 0
    (*this->out_) << "    jmp " << end << "\n";
    (*this->out_) << t << ":\n";
    (*this->out_) << "    mov fh r0 r" << reg << " 1\n";   // 真: r{reg} = 1
    (*this->out_) << end << ":\n";
}

// 論理 && / || を短絡評価し，結果(0/1)をr{reg}に生成する
// && は偽で短絡(両方真で1)，|| は真で短絡(両方偽で0)．偽は整数なら0，ポインタならnullptr
void Generator::gen_logical(node_t *expr, int reg) {
    // r{reg+1}を使うため，上限(r15)を超えないことを確認する
    if (reg + 1 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at ")
              + loc_to_string(expr->loc);
    }
    const bool is_and = (expr->sval == "&&");
    const std::string shortcut = this->new_label();   // 短絡時の飛び先
    const std::string end = this->new_label();
    // 短絡判定のF系: && は「偽なら短絡(eq)」, || は「偽以外なら短絡(ne)」
    const std::string br = is_and ? "eq" : "ne";

    // 左を評価．短絡条件を満たせば右を評価する命令を飛ばして結果へ行く
    this->gen_expr(expr->children[0], reg);
    (*this->out_) << "    mov fh r0 r" << (reg + 1) << " " << false_literal(expr->children[0]->type) << "\n";
    (*this->out_) << "    " << br << " r" << reg << " r" << (reg + 1) << " " << shortcut << "\n";
    // 右を評価．こちらは飛ばす対象が無いので短絡ではなく，結果(0/1)を確定させるための判定
    this->gen_expr(expr->children[1], reg);
    (*this->out_) << "    mov fh r0 r" << (reg + 1) << " " << false_literal(expr->children[1]->type) << "\n";
    (*this->out_) << "    " << br << " r" << reg << " r" << (reg + 1) << " " << shortcut << "\n";
    // どちらも短絡しなかった場合の結果 (&&なら1, ||なら0)
    (*this->out_) << "    mov fh r0 r" << reg << " " << (is_and ? 1 : 0) << "\n";
    (*this->out_) << "    jmp " << end << "\n";
    // 短絡した場合の結果 (&&なら0, ||なら1)
    (*this->out_) << shortcut << ":\n";
    (*this->out_) << "    mov fh r0 r" << reg << " " << (is_and ? 0 : 1) << "\n";
    (*this->out_) << end << ":\n";
}

// 三項演算子 a ? b : c の結果をr{reg}に生成する
void Generator::gen_ternary(node_t *expr, int reg) {
    const std::string else_label = this->new_label();
    const std::string end = this->new_label();
    this->gen_branch_if_false(expr->children[0], else_label, reg);   // 条件が偽ならelse値へ
    this->gen_expr(expr->children[1], reg);                          // then値 → r{reg}
    (*this->out_) << "    jmp " << end << "\n";
    (*this->out_) << else_label << ":\n";
    this->gen_expr(expr->children[2], reg);                          // else値 → r{reg}
    (*this->out_) << end << ":\n";
}

// インクリメント/デクリメント (++/--) を生成する
// 対象は読み書き可能なスカラーの左辺値 (アナライザが保証)．前置は増減後の新値・後置は増減前の旧値を式の値とする
// 1回で動かす量はアナライザが注釈済み (整数は1，ポインタは指す先1個分のバイト数)
void Generator::gen_incdec(node_t *expr, int reg, bool is_prefix) {
    // r{reg+1}を使うため，上限(r15)を超えないことを確認する
    if (reg + 1 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at ")
              + loc_to_string(expr->loc);
    }
    node_t *target = expr->children[0];                       // 対象 (変数・配列要素・構造体メンバ・間接参照)
    const std::string op = (expr->sval == "++") ? "+" : "-";  // ++→加算, --→減算

    // 番地が実行時に決まる対象の場合 (番地を一度だけ求め，読み出しと書き戻しの両方に使い回す．
    //  添字に関数呼び出しなどの副作用があっても，求め直すと2回評価してしまうため)
    if (has_runtime_addr(target)) {
        // レジスタ使用: r{reg}=現在値，r{reg+1}=番地，r{reg+2}=増減量→後置の新値
        // 作業用のr{reg+2}が上限(r15)を超えないことを確認する
        if (reg + 2 >= MAX_REG) {
            throw std::string("compiler error: expression too complex (out of registers) at ")
                  + loc_to_string(expr->loc);
        }
        // 増減する対象(変数・配列要素・メンバ・ポインタの指す先)が置かれているメモリ番地をr{reg+1}に求める
        this->gen_lvalue_addr(target, reg + 1);
        // 番地をr{reg}へ複製する (読み込みは結果を読み込み元と同じレジスタに上書きするため)
        (*this->out_) << "    mov fh r" << (reg + 1) << " r" << reg << "\n";           // r{reg} = 番地
        // 現在値をr{reg}へ読み込む (char/shortの符号拡張は，r{reg+1}の番地を壊さないようr{reg+2}を作業用に使う)
        this->gen_load_indirect(reg, target->type, reg + 2, target->loc);              // r{reg} = x
        (*this->out_) << "    mov fh r0 r" << (reg + 2) << " " << expr->step << "\n";  // r{reg+2} = 増減量
        // 増減した値を書き戻す．加減算は符号によって命令が変わらないため，
        // 加減算の命令生成には符号付きかどうかを常に真として渡す
        if (is_prefix) {
            // 前置 ++x/--x : r{reg}を増減して書き戻す (新値がそのまま式の値として残る)
            this->gen_binop_instr(op, true, reg, reg, reg + 2);                        // r{reg} = x ± 増減量
            this->gen_store_indirect(reg + 1, reg, target->type);                      // x = r{reg}
        } else {
            // 後置 x++/x-- : 旧値をr{reg}に残したまま，新値をr{reg+2}で計算して書き戻す
            this->gen_binop_instr(op, true, reg + 2, reg, reg + 2);                    // r{reg+2} = x ± 増減量
            this->gen_store_indirect(reg + 1, reg + 2, target->type);                  // x = r{reg+2}
        }
        return;
    }

    // 番地が固定の対象(変数・構造体変数のメンバ)の場合: 現在値を読み，増減量を載せる
    this->gen_load(reg, target->sym, target->loc);                                  // r{reg} = x
    (*this->out_) << "    mov fh r0 r" << (reg + 1) << " " << expr->step << "\n";   // r{reg+1} = 増減量

    // 増減した値を書き戻す．加減算は符号によって命令が変わらないため，
    // 加減算の命令生成には符号付きかどうかを常に真として渡す
    if (is_prefix) {
        // 前置 ++x/--x : r{reg}を増減して書き戻す (新値がそのまま式の値として残る)
        this->gen_binop_instr(op, true, reg, reg, reg + 1);                         // r{reg} = x ± 増減量
        this->gen_store(reg, target->sym);                                          // x = r{reg}
    } else {
        // 後置 x++/x-- : 旧値をr{reg}に残したまま，新値をr{reg+1}で計算して書き戻す
        this->gen_binop_instr(op, true, reg + 1, reg, reg + 1);                     // r{reg+1} = x ± 増減量
        this->gen_store(reg + 1, target->sym);                                      // x = r{reg+1}
    }
}

// 単項演算 (-/+/~/!) を生成する
void Generator::gen_unary(node_t *expr, int reg) {
    const std::string &op = expr->sval;

    this->gen_expr(expr->children[0], reg);   // オペランド → r{reg}

    if (op == "+") {
        // 単項+ : 値はオペランドそのもの (何もしない)
        return;
    }
    else if (op == "-") {
        // 単項- : 0 - x で符号反転する (r{reg+1}を使うため上限(r15)を超えないことを確認する)
        if (reg + 1 >= MAX_REG) {
            throw std::string("compiler error: expression too complex (out of registers) at ")
                  + loc_to_string(expr->loc);
        }
        (*this->out_) << "    mov fh r0 r" << (reg + 1) << " 0\n";                          // r{reg+1} = 0
        (*this->out_) << "    sub r" << (reg + 1) << " r" << reg << " r" << reg << "\n";    // r{reg} = 0 - x
    } else if (op == "~") {
        // ビット反転 : NOT命令 (not rs1 rd)
        (*this->out_) << "    not r" << reg << " r" << reg << "\n";                         // r{reg} = ~x
    } else if (op == "!") {
        // 論理否定 : xが偽(整数は0，ポインタはnullptr)なら1，それ以外は0
        // (比較と同じ0/1生成パターン，r{reg+1}を使うため上限(r15)を超えないことを確認する)
        if (reg + 1 >= MAX_REG) {
            throw std::string("compiler error: expression too complex (out of registers) at ")
                  + loc_to_string(expr->loc);
        }
        const std::string t = this->new_label();      // 真(xが偽)の飛び先
        const std::string end = this->new_label();
        (*this->out_) << "    mov fh r0 r" << (reg + 1) << " " << false_literal(expr->children[0]->type) << "\n";  // r{reg+1} = 偽の値
        (*this->out_) << "    eq r" << reg << " r" << (reg + 1) << " " << t << "\n";                               // xが偽なら .Lt へ
        (*this->out_) << "    mov fh r0 r" << reg << " 0\n";                                                       // xが偽でない: r{reg} = 0
        (*this->out_) << "    jmp " << end << "\n";
        (*this->out_) << t << ":\n";
        (*this->out_) << "    mov fh r0 r" << reg << " 1\n";                                                       // xが偽: r{reg} = 1
        (*this->out_) << end << ":\n";
    } else {
        throw std::string("compiler error: unsupported unary operator '") + op
              + "' at " + loc_to_string(expr->loc);
    }
}

// if文を生成する
// children: [0]=条件, [1]=then節, [2]=else節(省略可)
void Generator::gen_if(node_t *stmt) {
    node_t *cond = stmt->children[0];
    const bool has_else = stmt->children.size() == 3;

    if (!has_else) {
        // if (cond) then : 偽なら本体を飛ばす
        const std::string end = this->new_label();
        this->gen_branch_if_false(cond, end);
        this->gen_stmt(stmt->children[1]);
        (*this->out_) << end << ":\n";
    } else {
        // if (cond) then else else節 : 偽ならelseへ，thenの後はelseを飛ばす
        const std::string else_label = this->new_label();
        const std::string end = this->new_label();
        this->gen_branch_if_false(cond, else_label);
        this->gen_stmt(stmt->children[1]);
        (*this->out_) << "    jmp " << end << "\n";
        (*this->out_) << else_label << ":\n";
        this->gen_stmt(stmt->children[2]);
        (*this->out_) << end << ":\n";
    }
}

// while文を生成する
// children: [0]=条件, [1]=本体
void Generator::gen_while(node_t *stmt) {
    const std::string top = this->new_label();   // 条件判定の先頭 (continueの飛び先)
    const std::string end = this->new_label();   // ループ脱出先 (breakの飛び先)
    this->break_labels_.push_back(end);
    this->continue_labels_.push_back(top);

    (*this->out_) << top << ":\n";
    this->gen_branch_if_false(stmt->children[0], end);   // 条件が偽なら脱出
    this->gen_stmt(stmt->children[1]);                   // 本体
    (*this->out_) << "    jmp " << top << "\n";        // 先頭(条件)へ戻る
    (*this->out_) << end << ":\n";

    this->continue_labels_.pop_back();
    this->break_labels_.pop_back();
}

// for文を生成する
// children: [0]=初期化, [1]=条件, [2]=更新, [3]=本体 (各部は省略時nullptr)
void Generator::gen_for(node_t *stmt) {
    node_t *init = stmt->children[0];
    node_t *cond = stmt->children[1];
    node_t *update = stmt->children[2];
    node_t *body = stmt->children[3];

    const std::string top = this->new_label();    // 条件判定の先頭
    const std::string cont = this->new_label();   // continueの飛び先 (更新部)
    const std::string end = this->new_label();    // ループ脱出先 (break)

    // 初期化 (省略時はnullptr)．ループ前に1回だけ実行する
    if (init != nullptr) this->gen_stmt(init);

    this->break_labels_.push_back(end);
    this->continue_labels_.push_back(cont);

    (*this->out_) << top << ":\n";
    // 条件 (省略時は判定なし＝常にループ)
    if (cond != nullptr) this->gen_branch_if_false(cond, end);
    this->gen_stmt(body);                            // 本体
    (*this->out_) << cont << ":\n";                // continueはここ(更新部)へ来る
    if (update != nullptr) this->gen_stmt(update);   // 更新 (省略可)
    (*this->out_) << "    jmp " << top << "\n";    // 条件へ戻る
    (*this->out_) << end << ":\n";

    this->continue_labels_.pop_back();
    this->break_labels_.pop_back();
}

// do-while文を生成する
// children: [0]=本体, [1]=条件 (本体を実行してから末尾で条件判定する)
void Generator::gen_do_while(node_t *stmt) {
    const std::string top = this->new_label();    // ループ先頭 (本体)
    const std::string cont = this->new_label();   // continueの飛び先 (末尾の条件判定)
    const std::string end = this->new_label();    // 脱出先 (break)
    this->break_labels_.push_back(end);
    this->continue_labels_.push_back(cont);

    (*this->out_) << top << ":\n";
    this->gen_stmt(stmt->children[0]);             // 本体
    (*this->out_) << cont << ":\n";              // continueはここ(条件判定)へ来る
    this->gen_branch_if_true(stmt->children[1], top);   // 条件が真なら先頭へ戻る
    (*this->out_) << end << ":\n";

    this->continue_labels_.pop_back();
    this->break_labels_.pop_back();
}

// switch文を生成する
// children: [0]=条件式, [1..]=case/defaultラベルと文を平坦に並べたもの
// 意味解析がcase値をivalに畳み込み済み
void Generator::gen_switch(node_t *stmt) {
    // 各case/defaultにラベルを割り当てる
    std::map<node_t *, std::string> label_of;   // case/defaultノード → 飛び先ラベル
    std::string default_label;                  // default節のラベル (無ければ空)
    for (size_t i = 1; i < stmt->children.size(); i++) {
        node_t *c = stmt->children[i];
        if (c->kind == ND_CASE) {
            label_of[c] = this->new_label();
        } else if (c->kind == ND_DEFAULT) {
            default_label = this->new_label();
            label_of[c] = default_label;
        }
    }
    const std::string end = this->new_label();   // switch脱出先 (breakの飛び先)
    this->break_labels_.push_back(end);

    // ディスパッチ: 条件を一度r0に評価し，各caseと比較して一致したらそのラベルへ飛ぶ
    this->gen_expr(stmt->children[0], 0);   // r0 = 条件値
    for (size_t i = 1; i < stmt->children.size(); i++) {
        node_t *c = stmt->children[i];
        if (c->kind == ND_CASE) {
            (*this->out_) << "    mov fh r0 r1 " << imm_literal(c->ival) << "\n";    // r1 = case値
            (*this->out_) << "    eq r0 r1 " << label_of[c] << "\n";    // 一致ならそのcaseへ
        }
    }
    // どのcaseにも一致しなければ default へ (無ければ end へ)
    (*this->out_) << "    jmp " << (default_label.empty() ? end : default_label) << "\n";

    // 本体: case/defaultラベルを所定位置に置き，文を順に生成する (フォールスルーは自然に表現される)
    for (size_t i = 1; i < stmt->children.size(); i++) {
        node_t *c = stmt->children[i];
        if (c->kind == ND_CASE || c->kind == ND_DEFAULT) {
            (*this->out_) << label_of[c] << ":\n";
        } else {
            this->gen_stmt(c);
        }
    }
    (*this->out_) << end << ":\n";

    this->break_labels_.pop_back();
}

// r{dst} = r{lhs} op r{rhs} となる演算命令を出力する (is_signedは符号付きで演算するか)
// 二項演算と複合代入で共用する (剰余だけはdiv/divuの4引数形式)
void Generator::gen_binop_instr(const std::string &op, bool is_signed, int dst, int lhs, int rhs) {
    // 剰余: div/divuは商をrdへ・余りをimmが指すレジスタ番地へ格納する
    // 商をr{rhs}に捨て，余りをr{dst}(番地dst)へ得る
    if (op == "%") {
        (*this->out_) << "    " << (is_signed ? "div" : "divu") << " r" << lhs << " r" << rhs
                        << " r" << rhs << " " << dst << "\n";
    } else {
        // それ以外は単一命令
        const std::string mn = binop_mnemonic(op, is_signed);     // 演算子→命令
        (*this->out_) << "    " << mn
                        << " r" << lhs << " r" << rhs << " r" << dst << "\n";
    }
}

// 変数の型に応じた，メモリの読み書きに使うmaskを返す
static const char *access_mask(const type_t &type) {
    // 配列そのものの場合 (配列は1つの値として読み書きできず，値として使う式は意味解析が先頭要素へのポインタに変換済み)
    if (type.is_array) {
        throw std::string("compiler error: array cannot be read or written as a single value in memory access");
    }
    // ポインタは指す先の型によらず，番地を保持する全32ビットを対象にする
    if (type.pointer_depth > 0) return "fh";
    // スカラーは型の幅のバイトだけを対象にする
    switch (type.base) {
        case BASE_CHAR:  return "1h";
        case BASE_SHORT: return "3h";
        case BASE_INT:   return "fh";
        default:
            throw std::string("compiler error: unsupported scalar type in memory access");
    }
}

// フレーム上の変数の，フレームの基準(SP)から数えたオフセットを返す
int Generator::calc_frame_offset(const symbol_t *sym) const {
    // 引数はローカル変数領域とレジスタ退避領域の後ろに続く
    if (sym->location == LOC_PARAM) {
        return this->local_size_ + this->spill_size_ + sym->address;
    }
    // ローカル変数領域はフレームの基準から始まるため，オフセットがそのまま位置になる
    if (sym->location == LOC_LOCAL) {
        return sym->address;
    }
    // フレーム上に置かない変数(グローバル変数・レジスタ直結・コンパイル時定数)を渡された場合
    throw std::string("compiler error: '") + sym->name + "' is not placed on the stack frame";
}

// r{reg}の退避枠の，フレームの基準(SP)から数えたオフセットを返す
int Generator::calc_spill_offset(int reg) const {
    // 退避領域はローカル変数領域の後ろに続き，レジスタ番号ごとに1ワードの枠を持つ
    return this->local_size_ + reg * 4;
}

// 引数を書き込む位置の，呼び出し元の現在のSPから数えたオフセットを返す
int Generator::calc_arg_offset(int arg_count, int index) {
    // 現在のSPのすぐ下に戻り先アドレスの1ワードが積まれ，その下に引数が宣言順に並ぶ
    return -4 - arg_count * 4 + index * 4;
}

// 変数の値をr{reg}へ読み込む
void Generator::gen_load(int reg, const symbol_t *sym, const loc_t &loc) {
    // レジスタ直結(LED等のI/Oレジスタ)の場合
    if (sym->location == LOC_REGISTER) {
        // mov rs1=番地, rd=r{reg} : r{reg} = register[番地] (即値を付けないとレジスタ間コピーになる)
        (*this->out_) << "    mov fh r" << sym->address << " r" << reg << "\n";
        return;
    }
    const char *mask = access_mask(sym->type);   // 型幅に応じたmask
    // グローバル変数の場合
    if (sym->location == LOC_GLOBAL) {
        // rm: メモリ絶対番地からr{reg}へ読み込む (即値アドレス指定のためrs1のr0は無視される)
        (*this->out_) << "    rm " << mask << " r0 r" << reg << " " << sym->address << "\n";
    }
    // フレーム上の変数の場合
    else if (sym->location == LOC_LOCAL || sym->location == LOC_PARAM) {
        // rmr: SPにフレーム内オフセットを足した番地からr{reg}へ読み込む
        (*this->out_) << "    rmr " << mask << " " << SP_REGISTER << " r" << reg
                      << " " << this->calc_frame_offset(sym) << "\n";
    }
    // メモリにもレジスタにも置かれていない場合 (値を読み出す手段がない)
    else {
        throw std::string("compiler error: '") + sym->name
              + "' has no readable storage at " + loc_to_string(loc);
    }
    // 符号付きchar/shortの場合 (レジスタ上の値は常にintとして扱うため，上位を符号ビットで埋める．
    //  符号なしは読み込みがmask外の上位バイトを0で埋めており，そのままintの値になっている．
    //  ポインタは番地であり，指す先がchar/shortでも符号拡張しない)
    if (sym->type.pointer_depth == 0 && sym->type.is_signed) {
        if (sym->type.base == BASE_CHAR)  this->gen_sign_extend(reg, 8, reg + 1, loc);
        if (sym->type.base == BASE_SHORT) this->gen_sign_extend(reg, 16, reg + 1, loc);
    }
}

// r{reg}の値を変数へ書き込む
void Generator::gen_store(int reg, const symbol_t *sym) {
    // レジスタ直結(LED等のI/Oレジスタ)の場合
    if (sym->location == LOC_REGISTER) {
        // mov rs1=r{reg}, rd=番地 : register[番地] = r{reg}
        (*this->out_) << "    mov fh r" << reg << " r" << sym->address << "\n";
        return;
    }
    const char *mask = access_mask(sym->type);   // 型幅に応じたmask
    // グローバル変数の場合
    if (sym->location == LOC_GLOBAL) {
        // wm: r{reg}をメモリ絶対番地へ書き込む (即値アドレス指定のためrs1のr0は無視される)
        (*this->out_) << "    wm " << mask << " r0 r" << reg << " " << sym->address << "\n";
    }
    // フレーム上の変数の場合
    else if (sym->location == LOC_LOCAL || sym->location == LOC_PARAM) {
        // wmr: SPにフレーム内オフセットを足した番地へr{reg}を書き込む
        (*this->out_) << "    wmr " << mask << " " << SP_REGISTER << " r" << reg
                      << " " << this->calc_frame_offset(sym) << "\n";
    }
    // メモリにもレジスタにも置かれていない場合 (値を書き込む先がない)
    else {
        throw std::string("compiler error: '") + sym->name + "' has no writable storage";
    }
}

// r{reg}が指すメモリ番地から，型に応じたマスクでr{reg}へ読み込む(レジスタ間接アドレッシング，結果は同じレジスタに上書き)
// 符号拡張の作業用レジスタは呼び出し側が指定する(読み込み後も値を保持したいレジスタを避けられるようにするため)
void Generator::gen_load_indirect(int reg, const type_t &type, int work_reg, const loc_t &loc) {
    // 型の幅のバイトだけを読み込む
    (*this->out_) << "    rm " << access_mask(type) << " r" << reg << " r" << reg << "\n";
    // 符号付きchar/shortの場合 (レジスタ上の値は常にintとして扱うため，上位を符号ビットで埋める．
    //  ポインタは番地であり，指す先がchar/shortでも符号拡張しない)
    if (type.pointer_depth == 0 && type.is_signed) {
        if (type.base == BASE_CHAR)  this->gen_sign_extend(reg, 8, work_reg, loc);
        if (type.base == BASE_SHORT) this->gen_sign_extend(reg, 16, work_reg, loc);
    }
}

// r{val_reg}の値を，r{addr_reg}が指すメモリ番地へ型に応じたマスクで書き込む(レジスタ間接アドレッシング)
void Generator::gen_store_indirect(int addr_reg, int val_reg, const type_t &type) {
    (*this->out_) << "    wm " << access_mask(type) << " r" << addr_reg << " r" << val_reg << "\n";
}

// r{reg}の下位bitsビットを符号として32ビットへ符号拡張する(シフト量の保持にr{work_reg}を使う)
void Generator::gen_sign_extend(int reg, int bits, int work_reg, const loc_t &loc) {
    // 作業用レジスタが上限(r15)を超えないことを確認する (r16以降はSP等の汎用でないレジスタのため)
    if (work_reg >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at ")
              + loc_to_string(loc);
    }
    const int shift = 32 - bits;   // 値の最上位ビットをレジスタのMSBへ運ぶシフト量
    // シフト量を保存しておく
    (*this->out_) << "    mov fh r0 r" << work_reg << " " << shift << "\n";
    // 最上位ビットをMSBにシフトする
    (*this->out_) << "    sll r" << reg << " r" << work_reg << " r" << reg << "\n";
    // 算術シフトして，実際の値が入っているよりも上位のビットを符号ビットで埋める
    (*this->out_) << "    sra r" << reg << " r" << work_reg << " r" << reg << "\n";
}

// 式を評価し結果を指定レジスタに残す．評価対象の式が関数呼び出しを含む場合，
// 呼び出し先はr0から使い直すため，別に指定したレジスタ(複数可)の値をフレーム上の退避領域へ
// 退避してから評価し，評価後に復元する
// 退避先は呼び出しごとのフレーム内にあるため，呼び出し先が同じレジスタを退避しても
// 呼び出し元の退避値は壊れない
void Generator::gen_expr_protecting(node_t *expr, int reg, const std::vector<int> &protect_regs) {
    // 手順:
    //   1. 保護するレジスタの値をメモリへ退避する
    //   2. 式を評価する
    //   3. 退避した値をレジスタへ復元する

    // 関数呼び出しを含まない式はreg以上のレジスタしか使わず保護対象を壊さないため，退避せずに評価する
    if (!contains_call(expr)) {
        this->gen_expr(expr, reg);
        return;
    }
    // 1. 保護するレジスタの値を，レジスタ番号ごとに決まった退避枠へ書き出す
    for (const int protect_reg : protect_regs) {
        // 退避枠の大きさを決めるため，退避したレジスタのうち最大の番号を控える
        if (protect_reg > this->max_spill_reg_) this->max_spill_reg_ = protect_reg;
        (*this->out_) << "    wmr fh " << SP_REGISTER << " r" << protect_reg
                      << " " << this->calc_spill_offset(protect_reg) << "\n";
    }
    // 2. 式を評価する (呼び出し先がレジスタを使い直すため，保護するレジスタの値はここで壊れうる)
    this->gen_expr(expr, reg);
    // 3. 退避枠から値を読み戻し，保護するレジスタを評価前の値に戻す
    for (const int protect_reg : protect_regs) {
        (*this->out_) << "    rmr fh " << SP_REGISTER << " r" << protect_reg
                      << " " << this->calc_spill_offset(protect_reg) << "\n";
    }
}

// 式を評価し，結果をr{reg}に残す
// reg以上のレジスタを作業用に使うレジスタスタック方式 (二項演算は左をr{reg}・右をr{reg+1}に評価して畳む)
void Generator::gen_expr(node_t *expr, int reg) {
    // レジスタは16本(r0〜r15)．深い式で枯渇したらエラーにする
    if (reg >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at ")
              + loc_to_string(expr->loc);
    }

    switch (expr->kind) {
        // リテラル: 即値をr{reg}に載せる
        // sizeofは意味解析でコンパイル時に値(ival)が確定済みのため，リテラルと同じ即値ロードで済む
        case ND_INT_LIT:
        case ND_CHAR_LIT:
        case ND_SIZEOF:
            (*this->out_) << "    mov fh r0 r" << reg << " " << imm_literal(expr->ival) << "\n";
            break;

        // nullptr: どの変数の番地とも関数の番地とも一致しない値をr{reg}に載せる
        case ND_NULLPTR:
            (*this->out_) << "    mov fh r0 r" << reg << " " << imm_literal(NULLPTR_VALUE) << "\n";
            break;

        // 関数の番地: 関数名を即値の位置に書き，アセンブラに関数の先頭indexへ解決させる
        case ND_FUNC_ADDR:
            (*this->out_) << "    mov fh r0 r" << reg << " " << expr->sval << "\n";
            break;

        // 文字列リテラル: 配列名と同様，先頭の番地(コンパイル時確定の即値)をr{reg}に載せる
        // データ自体はgen_global_initsで1回だけ書き込み済み
        // 値として使うと先頭要素へのポインタになるため，先頭アドレスだけ保存すればいい
        case ND_STRING_LIT:
            (*this->out_) << "    mov fh r0 r" << reg << " " << expr->sym->address << "\n";
            break;

        // 変数参照: 変数の値をr{reg}へ読み込む (配列を値として使う場合は，中身の代わりに先頭の番地を載せる)
        case ND_VAR:
            if (expr->is_decayed) {
                this->gen_var_addr(reg, expr->sym);
            } else {
                this->gen_load(reg, expr->sym, expr->loc);
            }
            break;

        // 構造体メンバ参照: メンバの値をr{reg}へ読み込む
        case ND_MEMBER_ACCESS:
            // メンバ配列を値として使う場合 (中身の代わりに先頭の番地を載せる)
            if (expr->is_decayed) {
                this->gen_lvalue_addr(expr, reg);
            }
            // 構造体配列要素のメンバ(arr[i].member)・構造体ポインタの指す先のメンバ(p->member)等の場合
            // (番地が実行時に決まるため，番地を計算してからレジスタ間接で読み込む)
            else if (has_runtime_addr(expr)) {
                // メンバの実アドレスをr{reg}に求める
                this->gen_lvalue_addr(expr, reg);
                // そのアドレスからメンバの値をr{reg}へ読み込む (符号拡張の作業用にr{reg+1}を使う)
                this->gen_load_indirect(reg, expr->type, reg + 1, expr->loc);
            }
            // 単一の構造体変数のメンバの場合 (意味解析が「構造体変数の番地+メンバのオフセット」を1つのシンボルに
            // 合成済みのため，通常の変数参照と同じ経路で読み込める)
            else {
                this->gen_load(reg, expr->sym, expr->loc);
            }
            break;

        // 番地の取得 &x: 左辺値の番地そのものを値とする
        case ND_ADDR_OF:
            this->gen_lvalue_addr(expr->children[0], reg);
            break;

        // 間接参照 *p: ポインタの値が指す番地から，指す先の型で読み込む
        case ND_DEREF:
            // 指す先の番地(ポインタ変数等に入っている値)をr{reg}に求める
            this->gen_lvalue_addr(expr, reg);
            // その番地のメモリから，指す先の型の幅で値をr{reg}へ読み込む (符号拡張の作業用にr{reg+1}を使う)
            this->gen_load_indirect(reg, expr->type, reg + 1, expr->loc);
            break;

        // 二項演算
        case ND_BINOP:
            // 比較は0/1の値を生成，論理&&/||は短絡評価，それ以外は単一命令(+など)で畳む
            if (is_comparison(expr->sval)) {
                this->gen_compare(expr, reg);
            } else if (expr->sval == "&&" || expr->sval == "||") {
                this->gen_logical(expr, reg);
            } else {
                // 左辺をr{reg}に評価する
                this->gen_expr(expr->children[0], reg);
                // 右辺をr{reg+1}に評価する (関数呼び出しを含む場合はr{reg}の左辺の値を保護する)
                this->gen_expr_protecting(expr->children[1], reg + 1, {reg});
                // ポインタを含む演算の場合 (ポインタの加減算・差は，指す先のバイト数を単位にする)
                if (is_pointer_like(expr->children[0]->type) || is_pointer_like(expr->children[1]->type)) {
                    this->gen_pointer_arith(expr, reg);
                }
                // 整数どうしの場合
                else {
                    // 左辺と右辺を演算子で畳み，結果をr{reg}に置く
                    this->gen_binop_instr(expr->sval, is_signed_binop(expr), reg, reg, reg + 1);   // r{reg} = r{reg} op r{reg+1}
                }
            }
            break;

        // 三項演算子 a ? b : c
        case ND_TERNARY:
            this->gen_ternary(expr, reg);
            break;

        // 前置単項演算: ++/--は増減，それ以外(-/+/~/!)は単項演算
        case ND_UNOP:
            if (expr->sval == "++" || expr->sval == "--") {
                this->gen_incdec(expr, reg, true);    // 前置
            } else {
                this->gen_unary(expr, reg);
            }
            break;

        // 後置単項演算: ++/--のみ (parserがND_POST_UNOPを作るのはこの2つだけ)
        case ND_POST_UNOP:
            this->gen_incdec(expr, reg, false);       // 後置
            break;

        // 関数呼び出し
        case ND_CALL:
            this->gen_call(expr, reg);
            break;

        // 組み込み関数print: char配列をヌル終端まで1文字ずつ出力するループを生成する
        case ND_PRINT:
            this->gen_print_string(expr->children[0]->sym, reg);
            break;

        // 組み込み関数scan: 標準入力を改行まで読み込み，char配列へヌル終端付きで格納するループを生成する
        case ND_SCAN:
            this->gen_scan_line(expr->children[0]->sym, reg);
            break;

        // 組み込み関数streq: 2つのchar配列の内容を比較し，一致すれば1，不一致なら0をr{reg}に生成する
        case ND_STREQ:
            this->gen_streq(expr->children[0]->sym, expr->children[1]->sym, reg);
            break;

        // 組み込み関数strcopy: 第2引数(src)の内容を第1引数(dst)へヌル終端付きでコピーする
        case ND_STRCOPY:
            this->gen_strcopy(expr->children[0]->sym, expr->children[1]->sym, reg);
            break;

        // 代入: 右辺(複合代入は左辺の現在値と右辺の演算結果)をr{reg}に求め，変数へ書き込む
        case ND_ASSIGN: {
            node_t *lhs = expr->children[0];   // 代入先(左辺)
            // ポインタへの複合代入(+=/-=)か (右辺の整数を指す先のバイト数倍してから足し引きする)
            const bool is_pointer_compound = expr->sval != "=" && is_pointer_value(lhs->type);
            // 番地が実行時に決まる代入先の場合
            if (has_runtime_addr(lhs)) {
                // 配列要素・構造体配列要素のメンバ・ポインタの指す先への代入: 番地が実行時計算のため，アドレスを求めてから読み書きする．
                // 式の評価はreg以上のレジスタしか使わない規約(レジスタスタック方式)のため，
                // アドレスは右辺(または現在値の読み出し)より後ろのレジスタ(r{reg+1})に置く
                // (先にr{reg+1}へアドレスを置いてしまうと，reg起点で評価する式が
                //  自身の作業用としてr{reg+1}を使い，アドレスを上書きしてしまう)．
                // レジスタ使用: r{reg}=右辺値/現在値，r{reg+1}=アドレス(アドレス計算・右辺の評価にr{reg+2}以降も使う)
                // 作業用のr{reg+2}が上限(r15)を超えないことを確認する
                if (reg + 2 >= MAX_REG) {
                    throw std::string("compiler error: expression too complex (out of registers) at ")
                          + loc_to_string(expr->loc);
                }
                if (expr->sval == "=") {
                    // 単純代入: 右辺を先にr{reg}へ評価してから，アドレスをr{reg+1}へ求める(r{reg}を保護)
                    // 右辺の値をr{reg}に求める
                    this->gen_expr(expr->children[1], reg);
                    // 代入先のアドレスをr{reg+1}に求める (添字の関数呼び出しからr{reg}の右辺の値を保護する)
                    this->gen_lvalue_addr(lhs, reg + 1, {reg});
                } else {
                    // 複合代入 x op= e : アドレスをr{reg+1}へ求め，現在値をr{reg}へ読む
                    // (char/shortの符号拡張は，r{reg+1}のアドレスを壊さないようr{reg+2}を作業用に使う)．
                    // 右辺の評価が関数呼び出しを含む場合，呼び出し先はr0から使い直すため
                    // r{reg}(現在値)・r{reg+1}(アドレス)の両方を保護して評価する
                    // 代入先のアドレスをr{reg+1}に求める
                    this->gen_lvalue_addr(lhs, reg + 1);
                    // アドレスをr{reg}へ複製する (読み込みは結果を読み込み元と同じレジスタに上書きするため)
                    (*this->out_) << "    mov fh r" << (reg + 1) << " r" << reg << "\n";  // r{reg} = アドレス
                    // 代入先の現在値をr{reg}へ読み込む
                    this->gen_load_indirect(reg, lhs->type, reg + 2, lhs->loc);             // r{reg} = 現在値
                    const std::string op = expr->sval.substr(0, expr->sval.size() - 1);    // "+=" → "+"
                    const bool is_signed = is_signed_operation(op, lhs->type, expr->children[1]->type);   // 符号付きで演算するか
                    // 右辺をr{reg+2}に評価する
                    this->gen_expr_protecting(expr->children[1], reg + 2, {reg, reg + 1}); // 右辺 → r{reg+2}
                    // ポインタへの+=/-=の場合 (右辺を指す先のバイト数倍する)
                    if (is_pointer_compound) this->gen_scale(reg + 2, reg + 3, expr->step, expr);
                    // 現在値と右辺を演算子で畳み，結果をr{reg}に置く
                    this->gen_binop_instr(op, is_signed, reg, reg, reg + 2);
                }
                // r{reg}の値を，r{reg+1}のアドレスへ型に応じたマスクで書き込む (代入式の値もr{reg}に残る)
                this->gen_store_indirect(reg + 1, reg, lhs->type);
            } else {
                // スカラー変数への代入
                if (expr->sval == "=") {
                    // 単純代入: 右辺をr{reg}に評価する
                    this->gen_expr(expr->children[1], reg);
                } else {
                    // 複合代入 x op= e : 左辺の現在値をr{reg}・右辺をr{reg+1}に評価し，opで畳む
                    // 左辺の現在値をr{reg}に読み込む
                    this->gen_load(reg, lhs->sym, lhs->loc);
                    // 右辺をr{reg+1}に評価する (関数呼び出しを含む場合はr{reg}の現在値を保護する)
                    this->gen_expr_protecting(expr->children[1], reg + 1, {reg});
                    const std::string op = expr->sval.substr(0, expr->sval.size() - 1);   // "+=" → "+"
                    const bool is_signed = is_signed_operation(op, lhs->type, expr->children[1]->type);   // 符号付きで演算するか
                    // ポインタへの+=/-=の場合 (右辺を指す先のバイト数倍する)
                    if (is_pointer_compound) this->gen_scale(reg + 1, reg + 2, expr->step, expr);
                    // 現在値と右辺を演算子で畳み，結果をr{reg}に置く
                    this->gen_binop_instr(op, is_signed, reg, reg, reg + 1);
                }
                // 変数へ書き込む (代入式の値もr{reg}に残る)
                this->gen_store(reg, lhs->sym);
            }
            break;
        }

        // 配列要素アクセス(読み出し): インデックスからメモリアドレスを計算し，レジスタ間接で読み込む
        case ND_ARRAY_ACCESS:
            // 要素の実アドレスをr{reg}に求める
            this->gen_array_elem_addr(expr, reg);
            // そのアドレスから要素の値をr{reg}へ読み込む (符号拡張の作業用にr{reg+1}を使う)
            this->gen_load_indirect(reg, expr->type, reg + 1, expr->loc);
            break;

        default:
            throw std::string("compiler error: unsupported expression in code generation at ")
                  + loc_to_string(expr->loc);
    }
}

// 関数呼び出しを生成し，戻り値(戻り値のある関数の場合)をr{reg}に残す
// r{reg}以降のレジスタだけを使い，呼び出し元がr{reg}未満のレジスタに置いている値(二項演算の左辺等)を壊さない
void Generator::gen_call(node_t *expr, int reg) {
    // 手順:
    //   1. 関数ポインタを通した呼び出しなら，呼び出す関数の先頭番地をr{reg}に読み込む
    //   2. 書き込みを後回しにする引数の個数を決める
    //   3. 後回しにする引数をレジスタに残すのに必要な本数が足りるか確かめる
    //   4. 引数を順に評価する(引数の中で関数を呼ぶ場合は，評価済みの引数と呼び出す関数の番地を退避・復元する)
    //      後回しにしない引数は，評価した時点で書き込む
    //   5. 後回しにした引数を書き込む
    //   6. 呼び出し，戻り値を受け取る

    node_t *callee = expr->children[0];                                  // 呼び出す関数 (関数名または関数ポインタの値を持つ式)
    const func_sig_t &sig = *callee->type.func_sig;                      // 呼び出す関数のシグネチャ (引数を書き込む幅に使う)
    const int arg_count = static_cast<int>(expr->children.size()) - 1;   // 引数の個数
    const bool is_direct = callee->kind == ND_FUNC_ADDR;                 // 関数名による直接呼び出しか

    // 1. 関数ポインタを通した呼び出しの場合，関数ポインタの値(呼び出す関数の先頭番地)をr{reg}に読み込む
    //    関数名による直接呼び出しは，call命令に関数名を書くため読み込まない
    //    関数ポインタを求める式が関数呼び出しを含んでも，引数をSPより下へ書き込む前に評価し終えるため，
    //    書き込む引数を壊さない．番地はcall命令までr{reg}を占め続けるため，引数はその上のレジスタで評価する
    if (!is_direct) {
        this->gen_expr(callee, reg);
    }
    const int arg_reg_base = is_direct ? reg : reg + 1;                  // 引数の評価に使う先頭のレジスタ

    // 2. 引数を書き込む位置は現在のSPより下にあり，そこは次に呼ぶ関数の戻り先とフレームが
    //    占める領域でもある．f(1, g(2))を例にすると，1を書き込んだ位置はgのフレームに重なり，
    //    gが自分の引数の2やローカル変数を置いた時点で1が壊れる．このため，引数の中で
    //    関数を呼ぶ場合は，その引数までの書き込みを後回しにし，値をレジスタに残したまま評価する
    //    (f(1, g(2))なら，1とg(2)の戻り値をレジスタに持ったままgから戻り，そのあと両方を書き込む)
    int held_count = 0;   // 値をレジスタに残したままにする引数の個数
    for (int i = 0; i < arg_count; i++) {
        if (contains_call(expr->children[i + 1])) held_count = i + 1;
    }
    // 3. 残す引数はそれぞれ1本のレジスタを占めるため，その分だけ同時に使えるレジスタが必要になる
    //    (残りの引数は，書き込むまで値を保つ必要がないため，その上の1本を使い回す)
    if (arg_reg_base + std::min(arg_count - 1, held_count) >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at ")
              + loc_to_string(expr->loc);
    }

    // 4. 引数を宣言順に評価する (配列は先頭の番地を値として渡す)
    for (int i = 0; i < arg_count; i++) {
        node_t *arg = expr->children[i + 1];
        // レジスタに残す引数と，その上の1本で評価する引数とで使うレジスタを分ける
        // (同じレジスタを使うと，書き込む前の引数の値を壊してしまう)
        const int arg_reg = arg_reg_base + std::min(i, held_count);   // この引数を評価するレジスタ
        // 引数の中で呼ぶ関数に壊されないよう，この引数の評価の前後でメモリへ退避・復元するレジスタ
        // (呼び出す関数の番地と，評価済みでレジスタに残している引数)
        std::vector<int> protect_regs;
        // 関数ポインタを通した呼び出しの場合，呼び出す関数の番地を持つr{reg}を加える
        if (!is_direct) protect_regs.push_back(reg);
        // この引数より前に評価し，レジスタに残している引数のレジスタを加える
        for (int held = 0; held < i && held < held_count; held++) {
            protect_regs.push_back(arg_reg_base + held);
        }
        // この引数を評価する (退避・復元するレジスタの値は，評価の前後で保たれる)
        this->gen_expr_protecting(arg, arg_reg, protect_regs);
        // 以降に呼び出しが無い引数の場合 (上書きされないため，評価した時点で書き込む)
        if (i >= held_count) {
            this->gen_arg_store(arg_reg, sig.param_types[i], arg_count, i);
        }
    }
    // 5. レジスタに残しておいた引数を書き込む (引数の中で呼ぶ関数はすべて戻っている)
    for (int i = 0; i < held_count; i++) {
        this->gen_arg_store(arg_reg_base + i, sig.param_types[i], arg_count, i);
    }

    // 6. 呼び出す (関数ポインタを通した呼び出しは，r{reg}の値を呼び出す関数の先頭番地とする)
    if (is_direct) {
        (*this->out_) << "    call " << expr->sval << "\n";
    } else {
        (*this->out_) << "    call r" << reg << "\n";
    }
    // 戻り値を返す関数の場合 (RAXに置かれた戻り値を式の結果のレジスタへ取り出す)
    if (expr->type.base != BASE_VOID) {
        (*this->out_) << "    mov fh " << RAX_REGISTER << " r" << reg << "\n";
    }
}

// ポインタに整数を足し引きする演算・ポインタどうしの差をr{reg}に生成する (左辺はr{reg}・右辺はr{reg+1}に評価済み)
// 1増減するごとに動かすバイト数(指す先のバイト数)はアナライザがstepに注釈済み
void Generator::gen_pointer_arith(node_t *expr, int reg) {
    const bool is_lhs_pointer = is_pointer_value(expr->children[0]->type);   // 左辺がポインタか
    const bool is_rhs_pointer = is_pointer_value(expr->children[1]->type);   // 右辺がポインタか
    const long long bytes = expr->step;                                      // 指す先1個分のバイト数

    // ポインタどうしの差の場合 (番地の差を指す先のバイト数で割り，間の要素数にする)
    if (is_lhs_pointer && is_rhs_pointer) {
        (*this->out_) << "    sub r" << reg << " r" << (reg + 1) << " r" << reg << "\n";   // r{reg} = 番地の差
        // 1バイトなら割らない．2・4バイトは算術右シフト，それ以外(構造体．1個分のワード数×4のため8・12等)は除算で割る
        // (同じ配列を指すポインタどうしの差はバイト数で割り切れるため，シフトでも除算と同じ結果になる)
        if (bytes == 2 || bytes == 4) {
            (*this->out_) << "    mov fh r0 r" << (reg + 1) << " " << (bytes == 2 ? 1 : 2) << "\n";
            (*this->out_) << "    sra r" << reg << " r" << (reg + 1) << " r" << reg << "\n";
        } else if (bytes != 1) {
            (*this->out_) << "    mov fh r0 r" << (reg + 1) << " " << bytes << "\n";
            (*this->out_) << "    div r" << reg << " r" << (reg + 1) << " r" << reg << "\n";
        }
        return;
    }
    // ポインタ±整数・整数+ポインタの場合 (整数側を指す先のバイト数倍してから足し引きする)
    const int offset_reg = is_lhs_pointer ? reg + 1 : reg;   // 整数側を評価したレジスタ
    this->gen_scale(offset_reg, reg + 2, bytes, expr);
    const std::string mnemonic = (expr->sval == "+") ? "add" : "sub";   // 足すか引くか
    (*this->out_) << "    " << mnemonic << " r" << reg << " r" << (reg + 1) << " r" << reg << "\n";
}

// r{reg}の値(添字・ポインタに足し引きする整数)にbytesを掛け，番地のずれのバイト数に換算する (r{work_reg}を作業用に使う)
// 1倍は何もせず，2・4倍は左シフト，それ以外(構造体．1個分のワード数×4のため8・12等)は乗算で求める
void Generator::gen_scale(int reg, int work_reg, long long bytes, const node_t *expr) {
    // 1倍の場合 (値は変わらないため，これ以上何もしない)
    if (bytes == 1) return;
    // 作業用レジスタが上限(r15)を超えないことを確認する
    if (work_reg >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at ")
              + loc_to_string(expr->loc);
    }
    // 2・4倍は，2を底とする対数だけ左シフトする
    if (bytes == 2 || bytes == 4) {
        (*this->out_) << "    mov fh r0 r" << work_reg << " " << (bytes == 2 ? 1 : 2) << "\n";
        (*this->out_) << "    sll r" << reg << " r" << work_reg << " r" << reg << "\n";
        return;
    }
    // それ以外は乗算する (構造体1個分のバイト数は2の冪とは限らないため)
    (*this->out_) << "    mov fh r0 r" << work_reg << " " << bytes << "\n";
    (*this->out_) << "    mul r" << reg << " r" << work_reg << " r" << reg << "\n";
}

// r{reg}の値を，arg_count個の引数のindex番目を渡す位置へ書き込む
void Generator::gen_arg_store(int reg, const type_t &type, int arg_count, int index) {
    (*this->out_) << "    wmr " << access_mask(type) << " " << SP_REGISTER << " r" << reg
                  << " " << Generator::calc_arg_offset(arg_count, index) << "\n";
}

// 左辺値(書き込み先・&の対象)が置かれているメモリ番地をr{reg}に求める
// 変数は番地が固定のため即値またはSP相対で求め，それ以外は添字・ポインタの値から実行時に求める
void Generator::gen_lvalue_addr(node_t *target, int reg, const std::vector<int> &protect_regs) {
    switch (target->kind) {
        // 変数の場合 (構造体変数・配列変数を含む)
        case ND_VAR:
            this->gen_var_addr(reg, target->sym);
            return;
        // 配列要素の場合
        case ND_ARRAY_ACCESS:
            this->gen_array_elem_addr(target, reg, protect_regs);
            return;
        // 構造体メンバの場合 (基底の種類で番地の求め方が変わる)
        case ND_MEMBER_ACCESS: {
            const node_t *base = target->children[0];   // メンバが属する構造体の式
            // 構造体変数のメンバの場合 (意味解析が番地を合成したシンボルを持つ)
            if (base->kind == ND_VAR) {
                this->gen_var_addr(reg, target->sym);
            }
            // 構造体配列・構造体ポインタの変数に添字を付けた要素のメンバの場合
            else if (base->kind == ND_ARRAY_ACCESS && base->children.size() == 1) {
                this->gen_struct_array_member_addr(target, reg, protect_regs);
            }
            // 構造体ポインタの指す先・基底の式に添字を付けた要素のメンバの場合
            else if (base->kind == ND_DEREF || base->kind == ND_ARRAY_ACCESS) {
                this->gen_offset_member_addr(target, reg, protect_regs);
            }
            // メンバアクセスの基底になりえない式の場合 (構文解析が受け付けないため到達しない)
            else {
                throw std::string("compiler error: unsupported struct expression at ") + loc_to_string(target->loc);
            }
            return;
        }
        // 間接参照の場合 (ポインタの値がそのまま指す先の番地になる)
        case ND_DEREF:
            this->gen_expr_protecting(target->children[0], reg, protect_regs);
            return;
        // 番地を持たない式の場合 (意味解析が左辺値以外を受け付けないため到達しない)
        default:
            throw std::string("compiler error: expression has no address at ") + loc_to_string(target->loc);
    }
}

// 配列要素の実アドレスをr{reg}に計算する
// アドレス = 先頭番地 + index * 要素1個のバイト数．先頭番地は添字を付ける対象に応じて，配列ならその番地
// (コンパイル時定数・SP相対・メンバ配列の番地計算)，ポインタならその値(変数からの読み出し・式の評価)で求める
// レジスタ使用: r{reg}=index→アドレス, r{reg+1}=シフト量・先頭番地 (2本．先頭番地の計算(構造体配列要素の
// メンバ配列・ポインタを返す式等)がさらにr{reg+2}以降を使う場合がある)
// 読み書きの際の型に応じたマスクはここでは扱わない．求めたアドレスを使うレジスタ間接の読み込み・
// 書き込みの側が型から選ぶ(読み出しと書き込みでアドレス計算を共有するため)
void Generator::gen_array_elem_addr(node_t *expr, int reg, const std::vector<int> &protect_regs) {
    // 作業用のr{reg+2}が上限(r15)を超えないことを確認する
    if (reg + 2 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at ")
              + loc_to_string(expr->loc);
    }

    // r{reg} = index (r{reg+1}はまだ未使用)
    this->gen_expr_protecting(expr->children[0], reg, protect_regs);

    // オフセット = index * 要素1個のバイト数 (意味解析が注釈済み．サイズ1のcharはシフト不要)
    // 実行後: r{reg} = index * サイズ(バイトオフセット), r{reg+1} = シフト量(破棄可)
    this->gen_scale(reg, reg + 1, expr->step, expr);

    // 実行後: r{reg+1} = 先頭番地 (実行時計算の場合は，求めたオフセットのr{reg}も保護する)
    // 基底の式を持つ場合 (構造体のメンバ・間接参照・ポインタの配列の要素)
    if (expr->children.size() == 2) {
        node_t *base = expr->children[1];                   // 添字を付ける基底の式
        std::vector<int> base_protect_regs = protect_regs;  // 先頭番地の計算中に保護するレジスタ
        // 求めたオフセットを持つr{reg}も保護対象に加える
        base_protect_regs.push_back(reg);
        // メンバ配列ならその番地を，ポインタならその値を先頭番地としてr{reg+1}に求める
        if (base->type.is_array) {
            this->gen_lvalue_addr(base, reg + 1, base_protect_regs);
        } else {
            this->gen_expr_protecting(base, reg + 1, base_protect_regs);
        }
    }
    // ポインタ変数の場合 (指している番地を先頭番地としてr{reg+1}に読み込む)
    else if (is_pointer_value(expr->sym->type)) {
        this->gen_load(reg + 1, expr->sym, expr->loc);
    }
    // 配列変数の場合 (配列の先頭番地をr{reg+1}に求める)
    else {
        this->gen_var_addr(reg + 1, expr->sym);
    }
    // 実行後: r{reg} = 先頭番地 + オフセット = 実アドレス
    (*this->out_) << "    add r" << reg << " r" << (reg + 1) << " r" << reg << "\n";
}

// 変数の番地をr{reg}に載せる
void Generator::gen_var_addr(int reg, const symbol_t *sym) {
    // グローバル変数の場合 (番地がコンパイル時に確定しているため即値で載せる)
    if (sym->location == LOC_GLOBAL) {
        (*this->out_) << "    mov fh r0 r" << reg << " " << sym->address << "\n";
        return;
    }
    // フレーム上の変数(ローカル変数・引数)の場合 (実行時のSPにフレーム内オフセットを足して求める)
    if (sym->location == LOC_LOCAL || sym->location == LOC_PARAM) {
        (*this->out_) << "    mov fh r0 r" << reg << " " << this->calc_frame_offset(sym) << "\n";
        (*this->out_) << "    add " << SP_REGISTER << " r" << reg << " r" << reg << "\n";
        return;
    }
    // メモリ上に置かれていない場合 (レジスタ直結・コンパイル時定数は番地を持たない)
    throw std::string("compiler error: '") + sym->name + "' has no memory address";
}

// 構造体配列要素のメンバ(arr[i].member)の実アドレスをr{reg}に計算する
// アドレス = 先頭番地 + メンバオフセット(コンパイル時定数，member_accessが合成時にmember_offset_wordsへ保存済み)
//          + インデックス(実行時，member_access->children[0]->children[0]) × 構造体1要素分のバイト数
// 先頭番地は，構造体の配列なら配列の番地(コンパイル時定数にメンバオフセットを畳み込む)，
// 構造体へのポインタ(p[i].member)なら指している番地(実行時に読み出す)
// レジスタ使用: r{reg}=インデックス→アドレス, r{reg+1}=定数(要素間隔・ベース・メンバオフセット)・ポインタの値(作業用)
void Generator::gen_struct_array_member_addr(node_t *member_access, int reg, const std::vector<int> &protect_regs) {
    if (reg + 1 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at ")
              + loc_to_string(member_access->loc);
    }
    const symbol_t *arr_sym = member_access->sym;          // 配列全体・構造体ポインタのシンボル(先頭の位置・構造体名)
    node_t *array_access = member_access->children[0];     // arr[i] (children[0]=インデックス式)
    const int stride_bytes = this->analysis_.struct_defs.at(arr_sym->type.struct_name).total_words * 4;
    const bool is_pointer = is_pointer_value(arr_sym->type);   // 構造体へのポインタか
    const bool is_global = arr_sym->location == LOC_GLOBAL;    // 先頭が絶対番地で決まるか
    // 構造体の配列で，グローバル変数でもローカル変数でもない置き場所の場合 (構造体配列は他の置き場所を持たない)
    if (!is_pointer && !is_global && arr_sym->location != LOC_LOCAL) {
        throw std::string("compiler error: '") + arr_sym->name
              + "' is not placed in memory at " + loc_to_string(member_access->loc);
    }

    // r{reg} = インデックス式 (保護するレジスタが指定されていれば，それらの値を評価中も保護する)
    this->gen_expr_protecting(array_access->children[0], reg, protect_regs);
    // r{reg+1} = 要素間隔(構造体1要素分のバイト数．2の冪とは限らないためmulで乗算する)
    (*this->out_) << "    mov fh r0 r" << (reg + 1) << " " << stride_bytes << "\n";
    (*this->out_) << "    mul r" << reg << " r" << (reg + 1) << " r" << reg << "\n";

    // 構造体へのポインタの場合 (指している番地に，添字の分のずれとメンバのオフセットを足す)
    if (is_pointer) {
        // r{reg+1} = ポインタが指している番地
        this->gen_load(reg + 1, arr_sym, member_access->loc);
        (*this->out_) << "    add r" << reg << " r" << (reg + 1) << " r" << reg << "\n";
        // 先頭以外のメンバの場合 (メンバのオフセットを足す)
        if (member_access->member_offset_words != 0) {
            (*this->out_) << "    mov fh r0 r" << (reg + 1) << " " << (member_access->member_offset_words * 4) << "\n";
            (*this->out_) << "    add r" << reg << " r" << (reg + 1) << " r" << reg << "\n";
        }
        return;
    }

    // 配列の先頭の位置(グローバル変数は絶対番地，ローカル変数はフレーム内オフセット)にメンバのオフセットを足す
    const int base_const = (is_global ? arr_sym->address : this->calc_frame_offset(arr_sym))
                           + static_cast<int>(member_access->member_offset_words) * 4;
    // r{reg+1} = 配列先頭+メンバオフセット(コンパイル時定数)
    (*this->out_) << "    mov fh r0 r" << (reg + 1) << " " << base_const << "\n";
    // r{reg} = 実アドレス
    (*this->out_) << "    add r" << reg << " r" << (reg + 1) << " r" << reg << "\n";
    // ローカル変数の場合 (ここまでの計算はフレーム内オフセットであり，実行時のSPを足して番地にする)
    if (!is_global) {
        (*this->out_) << "    add " << SP_REGISTER << " r" << reg << " r" << reg << "\n";
    }
}

// 基底の構造体の番地を実行時に求め，メンバのオフセットを足してメンバの実アドレスをr{reg}に計算する
// アドレス = 基底の構造体の番地 + メンバオフセット(コンパイル時定数，member_accessが合成時にmember_offset_wordsへ保存済み)
// 基底の構造体の番地は，構造体ポインタの指す先(p->member)ならポインタの値，
// 基底の式に添字を付けた要素(s.items[i].member)ならその要素の番地
// レジスタ使用: r{reg}=基底の番地→アドレス, r{reg+1}=メンバオフセット(作業用．先頭のメンバなら使わない)
void Generator::gen_offset_member_addr(node_t *member_access, int reg, const std::vector<int> &protect_regs) {
    // r{reg} = 基底の構造体の番地 (保護するレジスタが指定されていれば，それらの値を評価中も保護する)
    this->gen_lvalue_addr(member_access->children[0], reg, protect_regs);
    // 先頭のメンバの場合 (基底の番地がそのままメンバの番地になるため，これ以上何もしない)
    if (member_access->member_offset_words == 0) return;
    // 作業用のr{reg+1}が上限(r15)を超えないことを確認する
    if (reg + 1 >= MAX_REG) {
        throw std::string("compiler error: expression too complex (out of registers) at ")
              + loc_to_string(member_access->loc);
    }
    // r{reg} = 基底の番地 + メンバのオフセット
    (*this->out_) << "    mov fh r0 r" << (reg + 1) << " " << (member_access->member_offset_words * 4) << "\n";
    (*this->out_) << "    add r" << reg << " r" << (reg + 1) << " r" << reg << "\n";
}

// グローバル変数とスタックがメモリ容量に収まるか検査する
// スタックはメモリの上端から下へ，グローバル変数は0番地から上へ伸びるため，両者が重なると
// 互いの値を壊す(ハードウェアは検出しない)
void Generator::check_memory_usage() {
    std::set<std::string> path;                // 現在の探索経路(再帰の検出用)
    std::map<std::string, int> recorded;       // 関数ごとに求めたスタック使用量
    bool is_recursive = false;                 // 探索中に再帰を見つけたか
    // mainを起点に，呼び出しの経路をたどってスタック使用量を求める
    const int stack_bytes = this->calc_stack_bytes("main", path, recorded, is_recursive);
    // 再帰がある場合 (深さが実行時にしか決まらず使用量を見積もれないため，検査しない)
    if (is_recursive) return;

    if (this->analysis_.global_size + stack_bytes > RAM_SIZE) {
        throw std::string("compiler error: global variables (")
              + std::to_string(this->analysis_.global_size) + " bytes) and stack ("
              + std::to_string(stack_bytes) + " bytes) exceed memory capacity ("
              + std::to_string(RAM_SIZE) + " bytes)";
    }
}

// funcを呼び出してから戻るまでに使うスタックのバイト数(最大)を返す
// func自身のフレームに，呼び出し先の中で最も多く使うものの使用量を足す
int Generator::calc_stack_bytes(const std::string &func, std::set<std::string> &path,
                                std::map<std::string, int> &recorded, bool &is_recursive) {
    // 現在の経路に既にfuncがある場合 (直接・間接を問わず再帰であり，深さが定まらない)
    if (path.count(func)) {
        is_recursive = true;
        return 0;
    }
    // 求めた結果が記録済みの場合 (どの経路から到達しても同じ値になるため，そのまま使う．
    //  記録せずに毎回たどり直すと，呼び出し関係が枝分かれしてから合流する形で経路の数が指数的に増える)
    const auto found = recorded.find(func);   // 記録済みの使用量
    if (found != recorded.end()) return found->second;

    path.insert(func);   // 経路にfuncを追加してから呼び出し先を探索する

    int max_callee_bytes = 0;   // 呼び出し先のうち最も多いスタック使用量
    for (const std::string &callee : this->analysis_.call_graph.at(func)) {
        // 呼び出しごとに戻り先アドレス(4バイト)が積まれる
        const int callee_bytes = 4 + this->calc_stack_bytes(callee, path, recorded, is_recursive);
        if (callee_bytes > max_callee_bytes) max_callee_bytes = callee_bytes;
    }

    path.erase(func);   // 探索し終えたので経路から外す(他の呼び出し経路と共有しないため)
    const int bytes = this->func_frame_sizes_.at(func) + max_callee_bytes;   // funcを起点とした使用量
    // 再帰を見つけた探索の途中で得た値は，打ち切った経路の分だけ少なくなるため記録しない
    if (!is_recursive) recorded[func] = bytes;
    return bytes;
}
