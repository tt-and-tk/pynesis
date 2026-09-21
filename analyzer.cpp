#include <set>
#include <vector>

#include "analyzer.hpp"

// グローバル変数の配置開始アドレス (1変数=1ワード(4バイト)で順次割り当てる)
static const int g_global_base_addr = 0x0000000;

// ハードウェア変数の定義表 (ボードI/Oレジスタのみ公開，CPU内部レジスタは非公開)
// 読み書き可否はハードウェア実装(mypc/alu.svh)に従う．型はピンの状態を表すビット列として扱うため，全てunsigned int
static const std::vector<symbol_t> g_hw_vars = {
    // 名前        型                置き場所       番地   読み   書き
    {"BTN",       {BASE_INT, false}, LOC_REGISTER, 0x20, true,  false},  // タクトスイッチ
    {"DIPSW",     {BASE_INT, false}, LOC_REGISTER, 0x21, true,  false},  // DIPスイッチ
    {"LED",       {BASE_INT, false}, LOC_REGISTER, 0x22, false, true},   // LED
    {"RGBLED",    {BASE_INT, false}, LOC_REGISTER, 0x23, false, true},   // RGB LED
    {"PMOD_A",    {BASE_INT, false}, LOC_REGISTER, 0x24, true,  true},   // Pmod A
    {"PMOD_B",    {BASE_INT, false}, LOC_REGISTER, 0x25, true,  true},   // Pmod B
    {"AR8_13",    {BASE_INT, false}, LOC_REGISTER, 0x26, true,  true},   // Arduinoピン AR8～AR13
    {"AR_I2C",    {BASE_INT, false}, LOC_REGISTER, 0x27, true,  true},   // A，AR_SDA，AR_SCL
    {"AR0_7",     {BASE_INT, false}, LOC_REGISTER, 0x28, true,  true},   // Arduinoピン AR0～AR7
    {"AR_RST",    {BASE_INT, false}, LOC_REGISTER, 0x29, true,  false},  // Arduinoリセット
    {"AR_SPI",    {BASE_INT, false}, LOC_REGISTER, 0x2a, true,  true},   // AR_MISO，AR_SCK，AR_MOSI，AR_SS
    {"GPIO0_7",   {BASE_INT, false}, LOC_REGISTER, 0x2d, true,  true},   // GPIO0～7
    {"GPIO8_15",  {BASE_INT, false}, LOC_REGISTER, 0x2e, true,  true},   // GPIO8～15
    {"GPIO16_23", {BASE_INT, false}, LOC_REGISTER, 0x2f, true,  true},   // GPIO16～23
    {"GPIO24_27", {BASE_INT, false}, LOC_REGISTER, 0x30, true,  true},   // GPIO24～27
};

// 値を32ビットで折り返し，符号付きなら-2147483648〜2147483647，符号なしなら0〜4294967295の範囲に正規化する
static long long wrap32(long long value, bool is_signed) {
    const long long bits = value & 0xFFFFFFFFLL;   // 下位32ビット
    // 符号付きで最上位ビットが立っている場合は負の値として解釈する
    if (is_signed && bits > 0x7FFFFFFFLL) return bits - 0x100000000LL;
    return bits;
}

// 整数リテラルの値がint型になるかを返す (intで表せない0x80000000以上の値は16進でのみ書け，unsigned intになる)
static bool is_signed_literal(long long value) {
    return value <= 0x7FFFFFFFLL;
}

// 整数昇格後の型が符号付き(int)かどうかを返す
// 演算のオペランドは意味解析で値として検査済みのため，整数かポインタ(配列は先頭要素へのポインタに変換済み)に限られる
bool is_promoted_signed(const type_t &type) {
    // ポインタ・nullptrでないこと (番地は符号なしの値として比較する)
    return !is_pointer_like(type)
        // かつ，次のいずれかであること
        && (type.is_signed              // 符号付きの型 (signed int等)
            || type.base != BASE_INT);  // char/short (符号の有無によらずintへ昇格する)
}

// 値がポインタ(関数ポインタ・nullptrを含む)として扱われるかどうかを返す
bool is_pointer_like(const type_t &type) {
    return is_pointer_value(type) || type.base == BASE_NULLPTR;
}

// 二項演算を符号付きで行うかどうかを，各オペランドの昇格後の型が符号付きかどうかから返す
static bool is_signed_operation_by_signs(const std::string &op, bool is_lhs_signed, bool is_rhs_signed) {
    // シフトは左オペランドの型に従う (シフト量の型は結果に影響しない)
    if (op == "<<" || op == ">>") return is_lhs_signed;
    return is_lhs_signed && is_rhs_signed;
}

// 二項演算を符号付きで行うかどうかを返す
bool is_signed_operation(const std::string &op, const type_t &lhs, const type_t &rhs) {
    return is_signed_operation_by_signs(op, is_promoted_signed(lhs), is_promoted_signed(rhs));
}

// コンストラクタ: ASTを受け取る
Analyzer::Analyzer(node_t *root) : root_(root), next_addr_(g_global_base_addr), local_size_(0) {}

// 意味解析を実行してシンボルテーブルを返す
std::map<std::string, const symbol_t *> Analyzer::operator()() {
    // ハードウェア変数をあらかじめシンボルテーブルに登録する (静的領域の実体を直接指す)
    for (const symbol_t &hw : g_hw_vars) {
        this->symbols_[hw.name] = &hw;
    }

    // 1パス目: グローバル宣言の索引を作り，名前の重複を検査する
    this->index_global_decls();

    // 2パス目: const変数・構造体定義・グローバル変数の登録と関数名の収集を行う
    this->collect_globals();
    // ポインタ型のグローバル変数の初期化子を検査する (後方で宣言された変数・関数の番地も使えるよう，全グローバル変数の登録後に行う)
    this->check_global_pointer_inits();

    // プログラムの開始点となるmain関数が必要
    if (this->func_names_.find("main") == this->func_names_.end()) {
        throw std::string("compiler error: 'main' function is not defined");
    }

    // 3パス目: 各関数本体を検査する
    // (analyze_expr内のND_CALLケースが，通りがけに全関数の呼び出し先をcall_graph_へ記録する．
    //  ここまで完了した時点で，どの関数がどの関数を呼ぶかの記録がすべて出揃っている)
    this->analyze_functions();

    // 関数ポインタを通した呼び出しは呼び出し先が実行時に決まるため，番地を取得された全関数を呼びうるものとして
    // 呼び出しグラフに加える (スタック使用量を少なく見積もらないため)
    for (const std::string &caller : this->indirect_callers_) {
        this->call_graph_[caller].insert(this->addr_taken_funcs_.begin(), this->addr_taken_funcs_.end());
    }

    // グローバル変数・文字列リテラルだけでメモリを使い切っていないか確認する
    // (スタックはメモリの上端から下へ伸びるため，残りがなければ関数を1つも呼び出せない．
    //  スタックまで含めた容量の検査は，フレームの大きさが確定するコード生成で行う)
    if (this->next_addr_ > RAM_SIZE) {
        throw std::string("compiler error: global variables (")
              + std::to_string(this->next_addr_) + " bytes) exceed memory capacity ("
              + std::to_string(RAM_SIZE) + " bytes)";
    }

    return this->symbols_;
}

// コード生成が参照する解析結果を返す
// (グローバル変数は0番地から順に割り当てるため，割り当て後の次の番地がそのまま占有バイト数になる)
analysis_result_t Analyzer::result() const {
    return {this->func_params_, this->struct_defs_, this->func_local_sizes_,
            this->call_graph_, this->next_addr_};
}

// 変数1つ分の領域を確保し，その先頭のオフセット(ローカル)または絶対番地(グローバル)を返す
// ローカル変数はスタックフレーム上に確保するため，関数ごとにフレーム内のローカル変数領域の
// 先頭から数えたオフセットを割り当てる(実際の番地は実行時のSPに応じて決まる)
int Analyzer::alloc_var(int bytes, location_t location) {
    // フレーム上に置く変数の場合
    if (location == LOC_LOCAL) {
        const int offset = this->local_size_;   // 確保する領域のローカル変数領域内でのオフセット
        this->local_size_ += bytes;
        return offset;
    }
    // 絶対番地に置く変数の場合
    if (location == LOC_GLOBAL) {
        const int addr = this->next_addr_;   // 確保する領域の絶対番地
        this->next_addr_ += bytes;
        return addr;
    }
    // 領域を確保しない置き場所(レジスタ直結・コンパイル時定数等)を渡された場合
    throw std::string("compiler error: cannot allocate memory for this kind of variable");
}

// 1パス目: プログラム直下の宣言の索引を作り，名前の重複を検査する
// 後方で宣言された変数・構造体を宣言順によらず解決できるよう，宣言ノードを名前から引けるようにしておく．
// 変数(const変数を含む)・関数・ハードウェア変数は同じ名前空間，構造体名はそれとは別の名前空間として検査する
void Analyzer::index_global_decls() {
    for (node_t *child : this->root_->children) {
        // 構造体定義の場合 (構造体名は変数・関数とは別の名前空間のため，構造体名どうしでのみ重複を検査する)
        if (child->kind == ND_STRUCT_DECL) {
            // 同じ名前の構造体が定義済みの場合 (同じ名前の構造体を再定義することは禁止する)
            if (this->struct_decl_nodes_.count(child->sval)) {
                throw std::string("compiler error: redefinition of struct '") + child->sval
                      + "' at " + loc_to_string(child->loc);
            }
            this->struct_decl_nodes_[child->sval] = child;
            continue;
        }

        // 名前の重複チェック (変数・関数・ハードウェア変数の全てと衝突しないこと)
        if (this->symbols_.count(child->sval) || this->global_var_decls_.count(child->sval)
            || this->func_names_.count(child->sval)) {
            throw std::string("compiler error: redefinition of '") + child->sval
                  + "' at " + loc_to_string(child->loc);
        }
        // 変数宣言(const変数を含む)の場合
        if (child->kind == ND_VAR_DECL) {
            this->global_var_decls_[child->sval] = child;
        } else {
            // 関数定義: 関数名と戻り値型を登録する
            this->func_names_[child->sval] = child->type;
        }
    }
}

// 宣言ノードの型を確定させる (確定済みなら何もしない)
// 構造体型なら構造体定義を解決し，配列なら要素数を文字列リテラルの長さ，または定数式から計算して畳み込む
void Analyzer::resolve_decl_type(node_t *decl) {
    // 型に現れる構造体が定義済みか確認する
    this->check_type_exists(decl->type, decl->loc);
    // 構造体そのものの場合 (メンバ構成が確定しないとサイズを求められないため，先に構造体定義を解決する．
    //  構造体へのポインタは1ワードの番地であり，指す先の定義は解決しない．解決すると，自身を指すメンバ
    //  (struct Node *next)を持つ構造体の定義が自身に依存する循環になるため)
    if (decl->type.base == BASE_STRUCT && decl->type.pointer_depth == 0) {
        this->resolve_struct_def(decl->type.struct_name);
    }
    // スカラー，または要素数が確定済みの配列の場合
    // (スカラーは確定させる要素数を持たず，確定済みの配列は計算し直す必要がないため，これ以上何もしない．
    //  配列の要素数は正の値に限るため，0は未確定を表す)
    if (!decl->type.is_array || decl->type.array_size > 0) return;

    // 文字列リテラルで初期化されている場合 (要素数は文字列長から決まるため，定数式を計算しない)
    if (!decl->children.empty() && decl->children[0]->kind == ND_STRING_LIT) {
        // 文字列リテラルによる初期化: char msg[] = "hello";
        // char以外の配列(charへのポインタの配列を含む)の場合
        if (decl->type.base != BASE_CHAR || decl->type.pointer_depth > 0) {
            throw std::string("compiler error: string literal can only initialize char array at ")
                  + loc_to_string(decl->children[0]->loc);
        }
        // サイズは文字列長 + 1(ヌル終端)
        decl->type.array_size = static_cast<int>(decl->children[0]->sval.size()) + 1;
        return;
    }

    // サイズ明示の配列宣言: int table[10];
    this->begin_resolving(decl);
    const long long size = this->eval_const_expr(decl->children[0]).value;   // 配列の要素数
    // 要素数が正でない場合
    if (size <= 0) {
        throw std::string("compiler error: array size must be positive at ")
              + loc_to_string(decl->children[0]->loc);
    }
    // 要素数がintで表せない場合 (0x80000000以上の符号なしの値)
    if (size > 0x7FFFFFFFLL) {
        throw std::string("compiler error: array size is too large at ")
              + loc_to_string(decl->children[0]->loc);
    }
    decl->type.array_size = static_cast<int>(size);
    // サイズ式を畳み込み済みリテラルに置き換える
    node_t *folded = new node_t;
    folded->kind = ND_INT_LIT;
    folded->ival = size;
    folded->loc = decl->children[0]->loc;
    decl->children[0] = folded;
    this->end_resolving(decl);
}

// 構造体定義のメンバ構成を確定させ，struct_defs_に登録する (登録済みなら何もしない)
// メンバのオフセット(構造体先頭からのワード数)と構造体全体のワード数をここで確定させる
// 無名構造体に続けて即座に変数宣言されていた場合，その変数自体は別のND_VAR_DECLノードとして
// root_の子に並んでいる(パーサが生成)ため，ここでは構造体定義(ND_STRUCT_DECL)だけを扱えばよく，
// 変数宣言側は2パス目のcollect_globalsが通常の構造体変数宣言と同じ経路で処理する
void Analyzer::resolve_struct_def(const std::string &name) {
    // 構造体定義が登録済みの場合 (メンバ構成は確定済みで，同じ定義を計算し直す必要がないため，これ以上何もしない)
    if (this->struct_defs_.count(name)) return;

    const node_t *decl = this->struct_decl_nodes_.at(name);   // 構造体定義ノード
    // 構造体定義の解決を始める (メンバの配列サイズがこの構造体自身に依存する循環参照を検出する)
    this->begin_resolving(decl);

    // メンバがない状態から構造体定義を組み立てる
    struct_def_t def;
    def.total_words = 0;

    // メンバを宣言順に登録し，構造体先頭からのオフセットを決める
    for (node_t *member : decl->children) {
        for (const struct_member_t &registered : def.members) {
            // 登録済みのメンバと同じ名前の場合
            if (registered.name == member->sval) {
                throw std::string("compiler error: duplicate member '") + member->sval
                      + "' in struct '" + name + "' at " + loc_to_string(member->loc);
            }
        }

        // 配列メンバのサイズを定数式として確定する (変数宣言の配列サイズと同じ扱い)
        this->resolve_decl_type(member);

        const int words = member->type.is_array ? Analyzer::calc_array_words(member->type) : 1;   // メンバが占めるワード数
        // それまでのメンバの合計ワード数をオフセットとしてメンバを登録し，合計ワード数を進める
        def.members.push_back({member->sval, member->type, def.total_words});
        def.total_words += words;
    }

    // 組み立てた構造体定義を登録し，解決を終える
    this->struct_defs_[name] = def;
    this->end_resolving(decl);
}

// グローバルのconst変数を名前から解決してシンボルを返す (値が未確定なら初期化子を計算して登録する)
// 該当するグローバルのconst変数の宣言がなければnullptrを返す
const symbol_t *Analyzer::resolve_global_const(const std::string &name) {
    const auto it = this->global_var_decls_.find(name);
    // 該当する宣言がない，または通常の変数の宣言の場合 (解決すべきconst変数がないため，これ以上何もしない)
    if (it == this->global_var_decls_.end() || !it->second->type.is_const) return nullptr;

    node_t *decl = it->second;   // const変数の宣言ノード
    // 値が未確定の場合 (確定済みなら登録済みのシンボルをそのまま返す)
    if (decl->sym == nullptr) {
        this->begin_resolving(decl);
        symbol_t *sym = this->register_const_var(decl);
        this->symbols_[name] = sym;
        decl->sym = sym;
        this->end_resolving(decl);
    }
    return decl->sym;
}

// 宣言の解決を始める
// 解決中の宣言に再び到達した場合は，宣言の型・値が自身に依存しているため循環参照としてエラーにする
void Analyzer::begin_resolving(const node_t *decl) {
    // 解決中の宣言に再び到達した場合
    if (this->resolving_decls_.count(decl)) {
        // 無名構造体の名前はパーサが割り当てた内部名でソースに現れないため，無名であることを示す
        const std::string name = (decl->kind == ND_STRUCT_DECL && decl->sval[0] == '$')
                                     ? "anonymous struct" : "'" + decl->sval + "'";   // エラーメッセージに表示する宣言名
        throw std::string("compiler error: circular reference in declaration of ") + name
              + " at " + loc_to_string(decl->loc);
    }
    this->resolving_decls_.insert(decl);
}

// 宣言の解決を終える
void Analyzer::end_resolving(const node_t *decl) {
    this->resolving_decls_.erase(decl);
}

// 構造体型の変数1つ分(配列宣言ならその配列全体分)のアドレスを確保し，シンボルを生成して返す
// (シンボル表への格納自体はグローバル用のcollect_globals・ローカル用のanalyze_local_declが
//  それぞれ自分のシンボル表(symbols_・scopes_)へ行うため，この関数ではまだ行わない)
symbol_t *Analyzer::register_struct_var(node_t *decl, location_t location) {
    // 宣言されている構造体の定義と，構造体配列なら要素数を確定させる (通常の配列宣言のサイズ指定と同じ扱い)
    this->resolve_decl_type(decl);
    const int element_count = decl->type.is_array ? decl->type.array_size : 1;   // 配列の要素数 (配列でなければ1)

    // 構造体1個分のワード数×要素数ぶんの領域を確保し，メンバ構成込みの型情報を持つシンボルを生成する
    // (メンバのメモリレイアウトは../specification/compiler.mdの「構造体」節を参照)
    const int bytes = this->struct_defs_.at(decl->type.struct_name).total_words * 4 * element_count;   // 確保するバイト数
    symbol_t *sym = new symbol_t{decl->sval, decl->type, location, this->alloc_var(bytes, location), true, true};
    // 生成したシンボルは，呼び出し元がグローバル/ローカルいずれかのシンボル表へ格納する
    return sym;
}

// const変数の初期化子を定数式として計算し，値を持つシンボルを生成して返す
// メモリ番地は割り当てず，値は参照箇所(analyze_exprのND_VAR)で整数リテラルとして埋め込まれる
symbol_t *Analyzer::register_const_var(const node_t *decl) {
    const node_t *init = decl->children[0];                          // 初期化子の式
    const long long value = this->eval_const_expr(init).value;       // 初期化子を計算した値

    // 値を宣言した型の範囲に収める (範囲外の値を黙って切り詰めると，同じ型の通常の変数と値が食い違うため)
    long long min_value;   // 宣言した型で表せる最小値
    long long max_value;   // 宣言した型で表せる最大値
    switch (decl->type.base) {
        case BASE_CHAR:  min_value = -128LL;        max_value = 127LL;        break;
        case BASE_SHORT: min_value = -32768LL;      max_value = 32767LL;      break;
        case BASE_INT:   min_value = -2147483648LL; max_value = 2147483647LL; break;
        default:
            throw std::string("compiler error: unsupported const variable type at ")
                  + loc_to_string(decl->loc);
    }
    // 符号なし型の場合は，0〜(2^ビット幅-1)の範囲にする (符号付きの最大値の2倍+1が2^ビット幅-1になる)
    if (!decl->type.is_signed) {
        max_value = max_value * 2 + 1;
        min_value = 0;
    }
    // 値が型の範囲外の場合
    if (value < min_value || value > max_value) {
        throw std::string("compiler error: value of const variable '") + decl->sval
              + "' is out of range for its type at " + loc_to_string(init->loc);
    }

    // 値は32ビットのビット列としてintに保持し，読み出す際に型に応じて解釈する(const_symbol_value)
    // 読み取り専用のシンボルにすることで，代入・++/--・scan等の書き込みを既存の検査でエラーにする
    const int bits = static_cast<int>(static_cast<unsigned int>(value));   // 値の32ビットのビット列
    return new symbol_t{decl->sval, decl->type, LOC_CONST, bits, true, false};
}

// const変数のシンボルが保持する32ビットのビット列を，型に応じた値として返す
long long Analyzer::const_symbol_value(const symbol_t *sym) {
    // unsigned intは最上位ビットが立っていても正の値として解釈する (unsigned char/shortは常にintの正の範囲に収まる)
    if (!is_promoted_signed(sym->type)) {
        return static_cast<unsigned int>(sym->address);
    }
    return sym->address;
}

// 2パス目: プログラム直下を宣言順に走査し，const変数・構造体定義・グローバル変数の登録と関数名の収集を行う
// 先に全グローバルを登録することで，関数本体からの前方参照(後ろで宣言された変数の使用)を可能にする．
// 後方の宣言が先に参照された場合，その宣言はこの走査で到達するより前に参照した時点で解決済みになっている
// (名前の重複は1パス目のindex_global_declsで検査済み)
void Analyzer::collect_globals() {
    for (node_t *child : this->root_->children) {
        // 構造体定義: メンバ構成を確定させる
        if (child->kind == ND_STRUCT_DECL) {
            this->resolve_struct_def(child->sval);
        }
        // グローバル変数宣言: 初期化子を評価し，アドレスを割り当てて登録する
        else if (child->kind == ND_VAR_DECL) {
            if (child->type.is_const) {
                // const変数: 値を確定させて登録する (メモリ番地は割り当てない)
                this->resolve_global_const(child->sval);
            } else if (child->type.base == BASE_STRUCT && child->type.pointer_depth == 0) {
                // 構造体変数(配列宣言含む): 初期化子は非対応のため，メンバ構成に基づくアドレス確保のみ行う
                symbol_t *sym = this->register_struct_var(child, LOC_GLOBAL);
                this->symbols_[child->sval] = sym;
                child->sym = sym;
            } else if (is_pointer_value(child->type)) {
                // ポインタ変数: 型に構造体が現れる場合はその定義が済んでいるか確かめ，1ワードを割り当てて登録する
                this->check_type_exists(child->type, child->loc);
                symbol_t *sym = new symbol_t{child->sval, child->type, LOC_GLOBAL,
                                             this->alloc_var(4, LOC_GLOBAL), true, true};
                this->symbols_[child->sval] = sym;
                child->sym = sym;
                // 初期化子は，全グローバル変数の番地が決まってから検査する (後方で宣言された変数の番地も使えるようにするため)
            } else if (child->type.is_array) {
                // 配列の要素数を確定させる (先に参照されて解決済みなら何もしない)
                this->resolve_decl_type(child);
                // アドレスを割り当てて登録する (確保ワード数は型に応じて計算)
                const int bytes = Analyzer::calc_array_words(child->type) * 4;   // 配列が占めるバイト数
                symbol_t *sym = new symbol_t{child->sval, child->type, LOC_GLOBAL,
                                             this->alloc_var(bytes, LOC_GLOBAL), true, true};
                this->symbols_[child->sval] = sym;
                child->sym = sym;
            } else {
                // スカラー変数: 初期化子があればコンパイル時に計算し，リテラルに置き換える(定数畳み込み)
                if (!child->children.empty()) {
                    const const_value_t init = this->eval_const_expr(child->children[0]);   // 初期化子を計算した値
                    node_t *folded = new node_t;
                    folded->kind = ND_INT_LIT;
                    folded->ival = init.value;
                    folded->type = type_t{BASE_INT, init.is_signed};
                    folded->loc = child->children[0]->loc;
                    // 差し替え前の旧部分木はあえて解放しない
                    // (ASTは全ノードをdeleteせず，プロセス終了時のOS回収に任せる方針のため)
                    child->children[0] = folded;  // 初期化式の子要素を計算済みのリテラルで更新する
                }
                // ソース宣言のグローバル変数はnewでヒープ確保し解放しない (型に関係なく1変数=1ワード(4バイト)使う)
                symbol_t *sym = new symbol_t{child->sval, child->type, LOC_GLOBAL,
                                             this->alloc_var(4, LOC_GLOBAL), true, true};
                this->symbols_[child->sval] = sym;
                child->sym = sym;   // 宣言ノード自身もシンボルを指す (コード生成でアドレス参照に使う)
            }
        }
        // 関数定義: 各引数の名前・型・フレーム上の位置(シンボル)と，関数の引数の型の並び・戻り値型(シグネチャ)を登録する
        // (関数名・戻り値型は1パス目のindex_global_declsで登録済み)
        // 3パス目で関数本体を検査する際，本体より後ろに定義された関数の呼び出しも引数を検査できるよう，
        // 全関数のシグネチャをここ(2パス目)で揃えておく．3パス目(analyze_functions)は，ここで作った引数の
        // シンボルを，関数本体の名前解決で探す範囲(スコープ)に登録してから本体の文を検査する
        else if (child->kind == ND_FUNC_DEF) {
            // 戻り値型に構造体が現れる場合はその定義が済んでいるか確かめる
            this->check_type_exists(child->type, child->loc);
            auto sig = std::make_shared<func_sig_t>();   // この関数のシグネチャ
            sig->return_type = child->type;
            std::vector<const symbol_t *> params;
            for (size_t i = 0; i + 1 < child->children.size(); i++) {
                node_t *param = child->children[i];
                // 引数の型に構造体が現れる場合はその定義が済んでいるか確かめる
                this->check_type_exists(param->type, param->loc);

                // 同一関数内での引数名重複はエラー
                for (const symbol_t *p : params) {
                    if (p->name == param->sval) {
                        throw std::string("compiler error: duplicate parameter name '") + param->sval
                              + "' at " + loc_to_string(param->loc);
                    }
                }
                // 関数と同名の引数は禁止する (呼び出しの名前f(...)が関数と変数のどちらを指すかを一意にするため)
                if (this->func_names_.count(param->sval)) {
                    throw std::string("compiler error: '") + param->sval + "' is already declared as a function at "
                          + loc_to_string(param->loc);
                }
                // ハードウェア変数と同名の引数は禁止する (I/Oレジスタの誤上書き防止)
                const auto hw_it = this->symbols_.find(param->sval);
                if (hw_it != this->symbols_.end() && hw_it->second->location == LOC_REGISTER) {
                    throw std::string("compiler error: cannot shadow hardware register '") + param->sval
                          + "' at " + loc_to_string(param->loc);
                }

                // 引数はポインタ(番地を保持する)も含め1つにつき1ワードを宣言順に占める
                symbol_t *sym = new symbol_t{param->sval, param->type, LOC_PARAM,
                                             static_cast<int>(params.size()) * 4, true, true};
                param->sym = sym;
                params.push_back(sym);
                sig->param_types.push_back(param->type);
            }
            this->func_params_[child->sval] = params;
            this->func_sigs_[child->sval] = sig;
        }
    }
}

// コンパイル時に値が確定する定数式を計算して値を返す (定数畳み込み)
// 呼び出し元は定数式が要求される文脈 (配列サイズ・初期化子・case値等．一覧は../specification/compiler.mdの「定数式」節を参照)
// 実行時の演算と結果が一致するよう，整数昇格と符号の規則に従い，32ビットで折り返して計算する．
// 通常の変数の参照や関数呼び出しなど，コンパイル時に値が確定しない式を含む場合はエラーにする．
// const変数の参照は，その値として計算する．
// また sizeof(変数名) も例外で許可する．sizeofが必要とするのは変数の「値」ではなく「型のサイズ」であり，
// 型は変数の値と無関係にシンボルテーブルから分かるため，変数参照であってもコンパイル時に確定できるため
// (いずれも参照先が後方で宣言されたグローバルの宣言なら，その時点で宣言ノードから型・値を解決する)
const_value_t Analyzer::eval_const_expr(const node_t *expr) {
    // 整数リテラル
    if (expr->kind == ND_INT_LIT) {
        return {expr->ival, is_signed_literal(expr->ival)};
    }
    // 文字リテラルはintへ昇格した値
    if (expr->kind == ND_CHAR_LIT) {
        return {expr->ival, true};
    }

    // const変数の参照は値を返す
    if (expr->kind == ND_VAR) {
        const symbol_t *sym = this->lookup_symbol(expr->sval);
        // シンボル表にない場合 (未解決の後方のグローバルのconst変数でありうるため，宣言から解決を試みる)
        if (sym == nullptr) {
            sym = this->resolve_global_const(expr->sval);
        }
        // シンボル表にも宣言にもない名前の場合 (未宣言なのでエラーにする)
        if (sym == nullptr && !this->global_var_decls_.count(expr->sval)) {
            throw std::string("compiler error: use of undeclared identifier '") + expr->sval
                  + "' at " + loc_to_string(expr->loc);
        }
        // const変数の場合
        if (sym != nullptr && sym->location == LOC_CONST) {
            return {Analyzer::const_symbol_value(sym), is_promoted_signed(sym->type)};
        }
        // 通常の変数は値がコンパイル時に確定しないので，「定数式でない」エラーとして扱う
    }

    // sizeof: 型名，または変数名の型サイズをコンパイル時に返す (式自体は評価しない．結果はint)
    if (expr->kind == ND_SIZEOF) {
        if (expr->children.empty()) {
            // sizeof(型名)
            return {type_size_bytes(expr->type, this->struct_defs_), true};
        }
        // sizeof(変数名): 値ではなく型だけが必要なのでND_VARのみ許可する
        const node_t *inner = expr->children[0];
        if (inner->kind != ND_VAR) {
            throw std::string("compiler error: sizeof argument in a constant expression "
                               "must be a type name or variable name at ")
                  + loc_to_string(inner->loc);
        }
        const symbol_t *sym = this->lookup_symbol(inner->sval);
        // シンボル表に登録済みの変数の場合
        if (sym != nullptr) {
            return {type_size_bytes(sym->type, this->struct_defs_), true};
        }
        // まだ登録されていないグローバルの宣言は，宣言ノードから型を確定させてサイズを求める
        const auto it = this->global_var_decls_.find(inner->sval);
        // グローバルの宣言もない場合
        if (it == this->global_var_decls_.end()) {
            throw std::string("compiler error: use of undeclared identifier '") + inner->sval
                  + "' at " + loc_to_string(inner->loc);
        }
        this->resolve_decl_type(it->second);
        return {type_size_bytes(it->second->type, this->struct_defs_), true};
    }

    // 前置単項演算
    if (expr->kind == ND_UNOP) {
        const const_value_t v = this->eval_const_expr(expr->children[0]);
        // -・+・~ の結果はオペランドの昇格後の型，! の結果はint
        if      (expr->sval == "-") return {wrap32(-v.value, v.is_signed), v.is_signed};
        else if (expr->sval == "+") return v;
        else if (expr->sval == "~") return {wrap32(~v.value, v.is_signed), v.is_signed};
        else if (expr->sval == "!") return {(v.value == 0) ? 1 : 0, true};
        // ++/-- は変数にしか使えないので定数式では不可 (下のエラーに落ちる)
    }

    // 二項演算
    if (expr->kind == ND_BINOP) {
        const const_value_t l = this->eval_const_expr(expr->children[0]);
        const const_value_t r = this->eval_const_expr(expr->children[1]);
        return Analyzer::eval_const_binop(expr, l, r);
    }

    // 三項演算: 結果の型は両分岐の値の型から決まるため，選ばれない分岐も計算する
    if (expr->kind == ND_TERNARY) {
        const const_value_t cond = this->eval_const_expr(expr->children[0]);
        const const_value_t then_value = this->eval_const_expr(expr->children[1]);
        const const_value_t else_value = this->eval_const_expr(expr->children[2]);
        const bool is_signed = then_value.is_signed && else_value.is_signed;                // 結果が符号付きか
        const long long chosen = (cond.value != 0) ? then_value.value : else_value.value;   // 選ばれた分岐の値
        return {wrap32(chosen, is_signed), is_signed};
    }

    // 通常の変数参照・関数呼び出し等はコンパイル時に値が確定しないのでエラー
    throw std::string("compiler error: expression must be a constant expression at ")
          + loc_to_string(expr->loc);
}

// 二項演算の定数式を，実行時の演算と同じ規則で計算する
// 符号なしの演算では，両辺を符号なし32ビットの値として解釈し直してから計算する
const_value_t Analyzer::eval_const_binop(const node_t *expr, const const_value_t &l, const const_value_t &r) {
    const std::string &op = expr->sval;   // 演算子
    const bool is_signed = is_signed_operation_by_signs(op, l.is_signed, r.is_signed);   // 符号付きで演算するか
    const long long lv = wrap32(l.value, is_signed);   // 演算の符号で解釈した左辺
    const long long rv = wrap32(r.value, is_signed);   // 演算の符号で解釈した右辺

    // 論理演算・比較の結果は0/1のint
    if (op == "&&") return {(lv != 0 && rv != 0) ? 1 : 0, true};
    if (op == "||") return {(lv != 0 || rv != 0) ? 1 : 0, true};
    if (op == "==") return {(lv == rv) ? 1 : 0, true};
    if (op == "!=") return {(lv != rv) ? 1 : 0, true};
    if (op == "<")  return {(lv < rv) ? 1 : 0, true};
    if (op == ">")  return {(lv > rv) ? 1 : 0, true};
    if (op == "<=") return {(lv <= rv) ? 1 : 0, true};
    if (op == ">=") return {(lv >= rv) ? 1 : 0, true};

    // 除算・剰余: 実行時にCPUが停止する，または結果が定まらない組み合わせはコンパイル時に検出する
    if (op == "/" || op == "%") {
        // ゼロ除算の場合
        if (rv == 0) {
            throw std::string("compiler error: division by zero at ") + loc_to_string(expr->loc);
        }
        // 符号付きの最小値を-1で割る場合 (商が32ビットで表せない)
        if (is_signed && lv == -2147483648LL && rv == -1) {
            throw std::string("compiler error: division overflow at ") + loc_to_string(expr->loc);
        }
    }

    // 乗算・左シフトは64ビットの符号付き整数でも桁あふれしうるため，符号なし64ビットで計算して下位32ビットを得る
    const unsigned long long ulv = static_cast<unsigned long long>(lv);   // 符号なし64ビットで扱う左辺
    const unsigned long long urv = static_cast<unsigned long long>(rv);   // 符号なし64ビットで扱う右辺
    long long result;   // 32ビットで折り返す前の結果
    if      (op == "+")  result = lv + rv;
    else if (op == "-")  result = lv - rv;
    else if (op == "*")  result = static_cast<long long>((ulv * urv) & 0xFFFFFFFFULL);
    else if (op == "/")  result = lv / rv;
    else if (op == "%")  result = lv % rv;
    else if (op == "&")  result = lv & rv;
    else if (op == "|")  result = lv | rv;
    else if (op == "^")  result = lv ^ rv;
    // シフト量は実行時と同じく下位5ビット(0〜31)のみを使う．右シフトは符号付きなら算術，符号なしなら論理になる
    else if (op == "<<") result = static_cast<long long>((ulv << (rv & 31)) & 0xFFFFFFFFULL);
    else if (op == ">>") result = lv >> (rv & 31);
    else {
        throw std::string("compiler error: expression must be a constant expression at ")
              + loc_to_string(expr->loc);
    }
    return {wrap32(result, is_signed), is_signed};
}

// 配列が占有するワード数を計算する
// (1要素のバイト数はint・ポインタが4，shortが2，charが1．要素を詰めて並べ，ワード単位に切り上げる)
int Analyzer::calc_array_words(const type_t &type) {
    const int n = type.array_size;
    // ポインタの配列の場合 (要素は番地を保持する1ワード)
    if (type.pointer_depth > 0) return n;
    switch (type.base) {
        case BASE_INT:   return n;             // 32ビット: 1要素=1ワード
        case BASE_SHORT: return (n + 1) / 2;   // 16ビット: 2要素=1ワード
        case BASE_CHAR:  return (n + 3) / 4;   // 8ビット: 4要素=1ワード
        default:
            throw std::string("compiler error: unsupported array element type");
    }
}

// 型の論理バイト数を返す (sizeof・ポインタ演算の単位用．C言語準拠で実メモリのワード境界は考慮しない)
// 配列は「要素数 × 要素型のバイト数」を返す．構造体はメンバの合計ワード数から求める(構造体定義の参照が必要)
int type_size_bytes(const type_t &type, const std::map<std::string, struct_def_t> &struct_defs) {
    // ポインタとその配列の場合 (指す先の型によらず，要素は番地を保持する4バイト．構造体型より先に判定する)
    if (type.pointer_depth > 0) {
        return type.is_array ? 4 * type.array_size : 4;
    }
    if (type.base == BASE_STRUCT) {
        // BASE_STRUCT型の型情報はresolve_decl_typeが構造体の存在を検証して定義を解決した後にしか
        // 使われないため(未定義の構造体はそこで既にコンパイルエラーになる)，ここに渡ってくる
        // typeのstruct_nameは常に登録済みであり，探索に失敗することはない．
        // 構造体配列は「構造体1個分のバイト数×要素数」を返す
        const int struct_bytes = struct_defs.at(type.struct_name).total_words * 4;
        return type.is_array ? struct_bytes * type.array_size : struct_bytes;
    }
    int elem_bytes;
    switch (type.base) {
        case BASE_CHAR:  elem_bytes = 1; break;
        case BASE_SHORT: elem_bytes = 2; break;
        case BASE_INT:   elem_bytes = 4; break;
        default:
            throw std::string("compiler error: sizeof of unsupported type");
    }
    return type.is_array ? elem_bytes * type.array_size : elem_bytes;
}

// 型に構造体名が現れる場合，その構造体が定義済みであることを確かめる (現れなければ何もしない)
// ポインタの指す先の構造体は宣言時に定義を解決しないため，名前が存在することだけをここで確かめる
void Analyzer::check_type_exists(const type_t &type, const loc_t &loc) const {
    // 構造体型(構造体へのポインタを含む)で，その構造体が定義されていない場合
    if (type.base == BASE_STRUCT && this->struct_decl_nodes_.count(type.struct_name) == 0) {
        throw std::string("compiler error: use of undeclared struct '") + type.struct_name
              + "' at " + loc_to_string(loc);
    }
    // 関数ポインタの場合 (戻り値型・引数型にも構造体へのポインタが現れうる)
    if (type.base == BASE_FUNC) {
        this->check_type_exists(type.func_sig->return_type, loc);
        for (const type_t &param_type : type.func_sig->param_types) {
            this->check_type_exists(param_type, loc);
        }
    }
}

// ポインタ型のグローバル変数の初期化子を検査する
// グローバル変数の初期化子は，mainより前に実行される処理が存在しないため，コンパイル時に値が決まる式に限る．
// ポインタの場合は整数の定数式の代わりに，グローバル変数・関数の番地(に整数定数を足し引きした式)とnullptrを許す．
// 値は整数のように畳み込まず，他の初期化子と同じくmainの先頭で番地を求めて書き込む
void Analyzer::check_global_pointer_inits() {
    for (node_t *decl : this->root_->children) {
        // 変数宣言でない・ポインタ型でない・初期化子を持たない，のいずれかに当てはまる場合は検査しない
        if (decl->kind != ND_VAR_DECL || !is_pointer_value(decl->type) || decl->children.empty()) continue;
        node_t *init = decl->children[0];   // 初期化子の式
        // 初期化子を値として検査し，名前解決と型注釈を行う
        this->analyze_value(init);
        // 初期化子の値を変数の型の格納先へ格納できるか検査する (整数の値・指す先の型が異なるポインタはエラー)
        Analyzer::check_assignable(decl->type, init, "initialization");
        // コンパイル時に番地が決まらない式の場合 (変数の値の参照・関数呼び出し・間接参照等)
        if (!Analyzer::is_address_constant(init)) {
            throw std::string("compiler error: initializer of global pointer '") + decl->sval
                  + "' must be nullptr or an address of a global variable or function at "
                  + loc_to_string(init->loc);
        }
    }
}

// コンパイル時に値が決まる番地の式かを返す
// (nullptr・関数の番地・グローバル配列や文字列リテラルの先頭・&グローバルの左辺値と，それらに整数定数を足し引きした式)
bool Analyzer::is_address_constant(const node_t *expr) {
    switch (expr->kind) {
        // nullptr・関数の番地・文字列リテラルの先頭の番地の場合 (常にコンパイル時に決まる)
        case ND_NULLPTR:
        case ND_FUNC_ADDR:
        case ND_STRING_LIT:
            return true;
        // 配列の先頭の番地として使われる変数 (グローバルスコープにはグローバル変数しかない)
        case ND_VAR:
            return expr->is_decayed;
        // メンバ配列の先頭の番地として使われるメンバ
        case ND_MEMBER_ACCESS:
            return expr->is_decayed && Analyzer::is_static_lvalue(expr);
        // &の場合 (対象の番地がコンパイル時に決まる左辺値ならよい)
        case ND_ADDR_OF:
            return Analyzer::is_static_lvalue(expr->children[0]);
        // ポインタに整数定数を足し引きした式の場合
        case ND_BINOP: {
            const node_t *lhs = expr->children[0];   // 左辺
            const node_t *rhs = expr->children[1];   // 右辺
            // 整数定数+ポインタの場合 (ポインタ側の番地がコンパイル時に決まればよい)
            if (expr->sval == "+" && Analyzer::is_integer_constant(lhs)) {
                return Analyzer::is_address_constant(rhs);
            }
            // ポインタ±整数定数の場合 (ポインタ側の番地がコンパイル時に決まり，整数側が整数定数ならよい)
            return (expr->sval == "+" || expr->sval == "-")
                && Analyzer::is_address_constant(lhs) && Analyzer::is_integer_constant(rhs);
        }
        // それ以外(変数の値の参照・関数呼び出し・間接参照等)の場合 (実行時に値が決まる)
        default:
            return false;
    }
}

// 番地がコンパイル時に決まる左辺値かを返す
// (グローバル変数と，その配列要素(添字が整数定数)・メンバ．グローバルスコープにはグローバル変数しかない)
bool Analyzer::is_static_lvalue(const node_t *expr) {
    switch (expr->kind) {
        // 変数の場合 (グローバルスコープで名前解決した変数はグローバル変数であり，番地が固定)
        case ND_VAR:
            return true;
        // 配列の要素の場合 (ポインタの指す先は実行時に決まるため，配列そのものの要素に限る)
        case ND_ARRAY_ACCESS: {
            const bool has_const_index = Analyzer::is_integer_constant(expr->children[0]);   // 添字が整数定数か
            // 配列変数の要素の場合
            if (expr->children.size() == 1) {
                return has_const_index && expr->sym->type.is_array;
            }
            // メンバ配列の要素の場合
            const node_t *base = expr->children[1];   // 添字を付ける基底の式
            return has_const_index && base->kind == ND_MEMBER_ACCESS && base->type.is_array
                && Analyzer::is_static_lvalue(base);
        }
        // 構造体のメンバの場合 (構造体変数か，構造体配列の要素のメンバに限る)
        case ND_MEMBER_ACCESS: {
            const node_t *base = expr->children[0];   // メンバが属する構造体の式
            return base->kind == ND_VAR
                || (base->kind == ND_ARRAY_ACCESS && Analyzer::is_static_lvalue(base));
        }
        // それ以外(ポインタの指す先等)の場合 (番地が実行時に決まる)
        default:
            return false;
    }
}

// 値がコンパイル時に決まる整数の式かを返す
// const変数は名前解決の時点で値のリテラルに置き換わっているため，リテラルと演算子だけを調べればよい
bool Analyzer::is_integer_constant(const node_t *expr) {
    switch (expr->kind) {
        // リテラル・sizeofの場合 (値が決まっている)
        case ND_INT_LIT:
        case ND_CHAR_LIT:
        case ND_SIZEOF:
            return true;
        // 単項演算 (++/--は変数を書き換えるため除く)
        case ND_UNOP:
            return expr->sval != "++" && expr->sval != "--" && Analyzer::is_integer_constant(expr->children[0]);
        // 二項演算・三項演算子の場合 (オペランドがすべて整数定数ならよい)
        case ND_BINOP:
            return Analyzer::is_integer_constant(expr->children[0]) && Analyzer::is_integer_constant(expr->children[1]);
        case ND_TERNARY:
            return Analyzer::is_integer_constant(expr->children[0]) && Analyzer::is_integer_constant(expr->children[1])
                && Analyzer::is_integer_constant(expr->children[2]);
        // それ以外(変数の値の参照・関数呼び出し等)の場合 (実行時に値が決まる)
        default:
            return false;
    }
}

// 3パス目: 各関数本体を検査する
// ND_FUNC_DEFのchildren = [param0, param1, ..., block] (引数がなければchildren[0]がブロック)
void Analyzer::analyze_functions() {
    for (node_t *child : this->root_->children) {
        if (child->kind != ND_FUNC_DEF) continue;

        // 現在解析中の関数名・戻り値型を記録する (呼び出しグラフ構築・return文の整合性検査用)
        this->current_function_ = child->sval;
        this->current_return_type_ = child->type;
        this->call_graph_[child->sval];   // 呼び出し先が無い関数もグラフに登録しておく(空集合)
        // ローカル変数はこの関数のフレーム内に確保するため，関数ごとにオフセットを0から数え直す
        this->local_size_ = 0;

        // 関数スコープを開く (引数と本体のローカル変数が同じスコープに入る)
        this->scopes_.push_back({});

        // 引数をスコープに登録する (シンボル自体は2パス目のcollect_globalsで作成済み)
        for (const symbol_t *sym : this->func_params_[child->sval]) {
            this->scopes_.back()[sym->name] = sym;
        }

        // 関数本体ブロック(最後の子)を検査する
        this->analyze_block(child->children.back());

        // 確定したローカル変数領域の大きさを記録する (コード生成がフレームの大きさを決めるのに使う)
        this->func_local_sizes_[child->sval] = this->local_size_;

        // 関数スコープを閉じる
        this->scopes_.pop_back();
    }
}

// ブロックを検査する (新しいローカルスコープを積み，抜けるときに捨てる)
void Analyzer::analyze_block(node_t *block) {
    this->scopes_.push_back({});   // 新しいスコープを積む
    for (node_t *stmt : block->children) {
        this->analyze_stmt(stmt);
    }
    this->scopes_.pop_back();      // スコープを捨てる
}

// 文を検査する
void Analyzer::analyze_stmt(node_t *stmt) {
    // 変数宣言
    if (stmt->kind == ND_VAR_DECL) {
        this->analyze_local_decl(stmt);
    }
    // ブロック (入れ子の { ... })
    else if (stmt->kind == ND_BLOCK) {
        this->analyze_block(stmt);
    }
    // return文: 関数の戻り値型とreturn文の有無・値が一致するか検査する
    else if (stmt->kind == ND_RETURN) {
        if (stmt->children.empty()) {
            // void関数はreturn文自体が任意なので，値なしのreturn;はvoid以外のときのみエラー
            if (this->current_return_type_.base != BASE_VOID) {
                throw std::string("compiler error: non-void function '") + this->current_function_
                      + "' must return a value at " + loc_to_string(stmt->loc);
            }
        } else {
            if (this->current_return_type_.base == BASE_VOID) {
                throw std::string("compiler error: void function '") + this->current_function_
                      + "' cannot return a value at " + loc_to_string(stmt->loc);
            }
            this->analyze_value(stmt->children[0]);
            // 戻り値を戻り値型の格納先へ格納できるか検査する (ポインタと整数の取り違え・指す先の型の違いをエラーにする．
            // 整数どうしは幅への切り詰めも行わずそのまま返す)
            Analyzer::check_assignable(this->current_return_type_, stmt->children[0], "return");
        }
    }
    // if文 (children: 条件, then節, [else節])
    else if (stmt->kind == ND_IF) {
        this->analyze_value(stmt->children[0]);    // 条件
        this->analyze_stmt(stmt->children[1]);     // then節
        if (stmt->children.size() == 3) {
            this->analyze_stmt(stmt->children[2]); // else節
        }
    }
    // while文 (children: 条件, 本体)
    else if (stmt->kind == ND_WHILE) {
        this->analyze_value(stmt->children[0]);    // 条件
        this->loop_depth_++;
        this->analyze_stmt(stmt->children[1]);     // 本体
        this->loop_depth_--;
    }
    // for文 (children: 初期化, 条件, 更新, 本体．省略された部分はnullptr)
    else if (stmt->kind == ND_FOR) {
        // for全体で1つのスコープを張る (初期化部で宣言した変数を条件・更新・本体から見えるようにする)
        this->scopes_.push_back({});
        if (stmt->children[0]) this->analyze_stmt(stmt->children[0]);  // 初期化
        if (stmt->children[1]) this->analyze_value(stmt->children[1]); // 条件
        if (stmt->children[2]) this->analyze_expr(stmt->children[2]);  // 更新
        this->loop_depth_++;
        this->analyze_stmt(stmt->children[3]);                         // 本体
        this->loop_depth_--;
        this->scopes_.pop_back();
    }
    // do-while文 (children: 本体, 条件)
    else if (stmt->kind == ND_DO_WHILE) {
        this->loop_depth_++;
        this->analyze_stmt(stmt->children[0]);     // 本体
        this->loop_depth_--;
        this->analyze_value(stmt->children[1]);    // 条件
    }
    // switch文
    else if (stmt->kind == ND_SWITCH) {
        this->analyze_switch(stmt);
    }
    // break文 (ループまたはswitchの中でのみ許される)
    else if (stmt->kind == ND_BREAK) {
        if (this->loop_depth_ == 0 && this->switch_depth_ == 0) {
            throw std::string("compiler error: 'break' outside loop or switch at ")
                  + loc_to_string(stmt->loc);
        }
    }
    // continue文 (ループの中でのみ許される)
    else if (stmt->kind == ND_CONTINUE) {
        if (this->loop_depth_ == 0) {
            throw std::string("compiler error: 'continue' outside loop at ")
                  + loc_to_string(stmt->loc);
        }
    }
    // それ以外は式文として検査する
    else {
        this->analyze_expr(stmt);
    }
}

// switch文を検査する (children: 条件式, 本体の文とcase/defaultラベルが平坦に並ぶ)
void Analyzer::analyze_switch(node_t *stmt) {
    this->analyze_value(stmt->children[0]);   // 条件式
    // 条件式がポインタの場合 (case値は整数定数であり，番地と比べる意味がないためエラーにする)
    if (is_pointer_like(stmt->children[0]->type)) {
        throw std::string("compiler error: switch condition must be an integer at ")
              + loc_to_string(stmt->children[0]->loc);
    }

    this->switch_depth_++;            // switchの中ではbreakが許される
    this->scopes_.push_back({});      // switch本体のスコープ

    std::set<long long> case_values;  // case値の32ビットのビット列 (重複検出用)
    bool has_default = false;         // defaultの重複検出用

    // 本体(children[1..])を順に検査する
    for (size_t i = 1; i < stmt->children.size(); i++) {
        node_t *child = stmt->children[i];
        // case節: 値は定数式．畳み込んで重複チェックし，結果をivalに保存する
        if (child->kind == ND_CASE) {
            const long long v = this->eval_const_expr(child->children[0]).value;
            // 条件の値とはビット列の一致で比較するため，ビット列が同じcase値を重複とみなす(-1と0xFFFFFFFF等)
            const long long bits = v & 0xFFFFFFFFLL;   // case値の32ビットのビット列
            if (case_values.count(bits)) {
                throw std::string("compiler error: duplicate case value at ")
                      + loc_to_string(child->loc);
            }
            case_values.insert(bits);
            child->ival = v;   // コード生成器が参照できるよう畳み込み結果を保存する
        }
        // default節: 重複は不可
        else if (child->kind == ND_DEFAULT) {
            if (has_default) {
                throw std::string("compiler error: multiple default labels at ")
                      + loc_to_string(child->loc);
            }
            has_default = true;
        }
        // それ以外は通常の文として検査する
        else {
            this->analyze_stmt(child);
        }
    }

    this->scopes_.pop_back();
    this->switch_depth_--;
}

// ローカル変数宣言を検査し，現在のスコープに登録する
void Analyzer::analyze_local_decl(node_t *decl) {
    // 同一スコープ内での二重宣言はエラー (外側スコープの同名はシャドーイングとして許容)
    if (this->scopes_.back().count(decl->sval)) {
        throw std::string("compiler error: redefinition of '") + decl->sval
              + "' at " + loc_to_string(decl->loc);
    }

    // 関数と同名のローカル変数は宣言できない (呼び出しの名前f(...)が関数と変数のどちらを指すかを一意にするため)
    if (this->func_names_.count(decl->sval)) {
        throw std::string("compiler error: '") + decl->sval + "' is already declared as a function at "
              + loc_to_string(decl->loc);
    }
    // ハードウェア変数と同名のローカル変数は宣言できない (I/Oレジスタを上書きしないように禁止する)
    const symbol_t *shadowed = this->lookup_symbol(decl->sval);
    if (shadowed != nullptr && shadowed->location == LOC_REGISTER) {
        throw std::string("compiler error: cannot redeclare hardware register '") + decl->sval
              + "' at " + loc_to_string(decl->loc);
    }

    if (decl->type.is_const) {
        // const変数: 初期化子を定数式として計算して値を持たせる (メモリ番地は割り当てない)
        // 登録より前に計算するため，初期化子中の同名の参照は外側のスコープの変数を指す
        symbol_t *sym = this->register_const_var(decl);
        this->scopes_.back()[decl->sval] = sym;
        decl->sym = sym;
    } else if (decl->type.base == BASE_STRUCT && decl->type.pointer_depth == 0) {
        // 構造体変数(配列宣言含む): 初期化子は非対応のため，メンバ構成に基づくアドレス確保のみ行う
        symbol_t *sym = this->register_struct_var(decl, LOC_LOCAL);
        this->scopes_.back()[decl->sval] = sym;
        decl->sym = sym;
    } else if (decl->type.is_array) {
        // 配列の要素数を確定させる (文字列リテラルの長さ，または定数式)
        this->resolve_decl_type(decl);
        // フレーム内のオフセットを割り当てて登録する
        const int bytes = Analyzer::calc_array_words(decl->type) * 4;   // 配列が占めるバイト数
        symbol_t *sym = new symbol_t{decl->sval, decl->type, LOC_LOCAL,
                                     this->alloc_var(bytes, LOC_LOCAL), true, true};
        this->scopes_.back()[decl->sval] = sym;
        decl->sym = sym;
    } else {
        // スカラー変数(ポインタを含む): 指す先の構造体が定義済みか確かめる
        this->check_type_exists(decl->type, decl->loc);
        // 初期化式があれば先に検査する (登録より前に行い，自己参照 int x = x; では外側のxを参照させる)
        if (!decl->children.empty()) {
            // 初期化子を値として検査し，名前解決と型注釈を行う
            this->analyze_value(decl->children[0]);
            // 初期化子の値を変数の型の格納先へ格納できるか検査する (ポインタと整数の取り違え・指す先の型の違いをエラーにする)
            Analyzer::check_assignable(decl->type, decl->children[0], "initialization");
        }
        // フレーム内のオフセットを割り当てて登録する (型に関係なく1変数=1ワード(4バイト)使う)
        symbol_t *sym = new symbol_t{decl->sval, decl->type, LOC_LOCAL,
                                     this->alloc_var(4, LOC_LOCAL), true, true};
        this->scopes_.back()[decl->sval] = sym;
        decl->sym = sym;   // 宣言ノード自身もシンボルを指す
    }
}

// scan以外の組み込み関数の引数をチェックする(print/streq/strcopy)
// scanは構文上，対象を識別子1個(TK_IDENT)に限定しているため構造体メンバ配列(arr[i].name)が
// 現れず，書き込み可能性・最小サイズ等の固有要件も持つため，この共通検査ではなく専用の検査を別途行う
// 引数は配列のまま検査する(先頭要素へのポインタとして扱うと，打ち切りに使う宣言サイズが分からなくなるため)
void Analyzer::check_char_array_operand(node_t *target, const std::string &builtin_name) {
    this->analyze_expr(target);
    // ポインタの場合 (指す先の配列の大きさが分からず，打ち切りの判定ができないため)
    if (is_pointer_like(target->type)) {
        throw std::string("compiler error: ") + builtin_name
              + " does not support pointers (the array size is unknown) at " + loc_to_string(target->loc);
    }
    // char型の配列でない場合はエラー (charへのポインタの配列は，要素が文字ではないため対象外)
    if (!target->type.is_array || target->type.base != BASE_CHAR || target->type.pointer_depth > 0) {
        throw std::string("compiler error: ") + builtin_name + " requires a char array at "
              + loc_to_string(target->loc);
    }
    // 番地が実行時に決まる構造体のメンバ配列(arr[i].name・p->name)は，コード生成が前提とする
    // 「コンパイル時に確定したアドレス」と相容れないためエラー
    if (target->kind == ND_MEMBER_ACCESS && target->children[0]->kind != ND_VAR) {
        throw std::string("compiler error: ") + builtin_name + " does not support an array member whose "
              "address is determined at run time at " + loc_to_string(target->loc);
    }
}

// 演算の対象(名前解決・型注釈済み)がスカラー(整数またはポインタ)であることを検査する
// 配列・構造体を丸ごと読み書きするコード生成の仕組みは無く，そのまま通すと先頭ワードだけを
// 読み書きするコードになる(構造体は内部エラーになる)ため，意味解析の段階でエラーにする
void Analyzer::check_scalar_operand(const node_t *target, const std::string &operation) {
    if (target->type.is_array || (target->type.base == BASE_STRUCT && target->type.pointer_depth == 0)) {
        throw std::string("compiler error: ") + operation + " is not supported for array or struct at "
              + loc_to_string(target->loc);
    }
}

// 評価した結果の値(整数・ポインタ)を使う式を検査する
// (代入の右辺・引数・戻り値・演算のオペランド等．書き込み先や&の対象として使う式はanalyze_lvalueで検査する)
// 値を持たないvoid(戻り値のない関数呼び出し)・構造体をエラーにし，配列は先頭要素へのポインタとして型を書き込む
// (配列を代入の右辺・引数・演算等に使うと，C言語と同じく先頭要素の番地を表す)
void Analyzer::analyze_value(node_t *expr) {
    this->analyze_expr(expr);
    // void値の場合
    if (expr->type.base == BASE_VOID) {
        throw std::string("compiler error: cannot use void value in expression at ") + loc_to_string(expr->loc);
    }
    // 構造体そのものの場合 (値をまとめて読み書きする仕組みが無いため．構造体の配列は先頭要素へのポインタになる)
    if (expr->type.base == BASE_STRUCT && expr->type.pointer_depth == 0 && !expr->type.is_array) {
        throw std::string("compiler error: struct cannot be used as a value; access a member or take its address at ")
              + loc_to_string(expr->loc);
    }
    // 配列の場合 (先頭要素へのポインタとして扱う)
    if (expr->type.is_array) {
        expr->type = decayed_type(expr->type);
        expr->is_decayed = true;
    }
}

// 書き込み先・番地の取得対象になる式(左辺値)を検査し，名前解決と型注釈を行う
// 値を読む側の検査(const変数の値への置き換え・読み取り可否)は当てない．書き込み先に当てると誤るため
void Analyzer::analyze_lvalue(node_t *expr) {
    // 変数の場合
    if (expr->kind == ND_VAR) {
        const symbol_t *sym = this->lookup_symbol(expr->sval);   // 変数のシンボル
        // 変数として見つからない場合 (関数名なら書き換えられない旨を，それ以外は未宣言である旨を示す)
        if (sym == nullptr) {
            const std::string reason = this->func_names_.count(expr->sval)
                                           ? "' is a function and cannot be modified at "
                                           : "' is not declared at ";   // エラーの理由
            throw std::string("compiler error: '") + expr->sval + reason + loc_to_string(expr->loc);
        }
        expr->sym  = sym;
        expr->type = sym->type;
        return;
    }
    // 配列要素・構造体メンバ・間接参照の場合 (それぞれのアクセスの検査に名前解決させる)
    if (expr->kind == ND_ARRAY_ACCESS || expr->kind == ND_MEMBER_ACCESS || expr->kind == ND_DEREF) {
        this->analyze_expr(expr);
        return;
    }
    // それ以外(リテラル・演算や関数呼び出しの結果等)は番地を持たない
    throw std::string("compiler error: expression is not a variable, array element, struct member or dereference at ")
          + loc_to_string(expr->loc);
}

// 関数呼び出しを検査する (children: [呼び出す関数(関数名または関数ポインタの値を持つ式), 引数...])
// 呼び出す関数が関数名なら直接呼び出し，それ以外は関数ポインタを通した呼び出しとし，
// どちらも呼び出す関数の型が持つシグネチャで引数と戻り値を検査する
void Analyzer::analyze_call(node_t *expr) {
    node_t *callee = expr->children[0];                                                      // 呼び出す関数
    const bool is_direct = callee->kind == ND_VAR && this->func_names_.count(callee->sval);  // 関数名による直接呼び出しか
    // 変数でも関数でもない名前の場合
    if (callee->kind == ND_VAR && !is_direct && this->lookup_symbol(callee->sval) == nullptr) {
        throw std::string("compiler error: call to undefined function '")
              + callee->sval + "' at " + loc_to_string(expr->loc);
    }
    // 直接呼び出しの場合 (関数と同名の変数は宣言できないため，関数名は常に関数を指す)
    if (is_direct) {
        // 呼び出す関数を関数の番地を表すノードに置き換えてその型を書き込み，呼び出しグラフに記録する
        // (番地を値として取得したわけではないため，関数ポインタを通して呼ばれうる関数には加えない)
        callee->kind = ND_FUNC_ADDR;
        callee->type = this->func_pointer_type(callee->sval);
        expr->sval = callee->sval;
        this->call_graph_[this->current_function_].insert(callee->sval);
    }
    // 関数ポインタを通した呼び出しの場合
    else {
        this->analyze_value(callee);
        // 関数ポインタでないものを呼び出した場合
        if (!is_func_pointer(callee->type)) {
            throw std::string("compiler error: called object is not a function or function pointer at ")
                  + loc_to_string(expr->loc);
        }
        this->indirect_callers_.insert(this->current_function_);
    }

    // 渡す引数の数が，呼び出す関数の引数の数と一致するか検証する
    const func_sig_t &sig = *callee->type.func_sig;       // 呼び出す関数のシグネチャ
    const size_t arg_count = expr->children.size() - 1;   // 引数の個数
    if (arg_count != sig.param_types.size()) {
        const std::string callee_name = is_direct ? "function '" + expr->sval + "'" : "function pointer";   // エラーメッセージに書く呼び出す関数
        throw std::string("compiler error: ") + callee_name + " expects "
              + std::to_string(sig.param_types.size()) + " argument(s) but got "
              + std::to_string(arg_count) + " at " + loc_to_string(expr->loc);
    }
    // 渡す各引数を，呼び出す関数の引数の型の格納先へ格納できるか検査する
    // (整数どうしは型が異なっても受け付け，呼び出す関数の引数の型で格納する．値の変換規則が決まっているため，型の一致は検査しない)
    for (size_t i = 0; i < arg_count; i++) {
        node_t *arg = expr->children[i + 1];   // i番目の引数
        // 引数を値として検査し，名前解決と型注釈を行う
        this->analyze_value(arg);
        // 引数の値を，呼び出す関数のi番目の引数の型の格納先へ格納できるか検査する
        Analyzer::check_assignable(sig.param_types[i], arg, "argument " + std::to_string(i + 1));
    }
    expr->type = sig.return_type;
}

// 二項演算を検査し，結果の型を注釈する
// 整数どうしは整数昇格の規則に従い，ポインタを含む演算は加減算・比較・論理演算のみを許す
void Analyzer::analyze_binop(node_t *expr) {
    this->analyze_value(expr->children[0]);
    this->analyze_value(expr->children[1]);
    const type_t &lhs = expr->children[0]->type;       // 左辺の型
    const type_t &rhs = expr->children[1]->type;       // 右辺の型
    const std::string &op = expr->sval;                // 演算子
    const bool is_lhs_pointer = is_pointer_like(lhs);  // 左辺がポインタ(nullptrを含む)か
    const bool is_rhs_pointer = is_pointer_like(rhs);  // 右辺がポインタ(nullptrを含む)か
    // 比較・論理演算か (結果は0/1のint)
    const bool is_boolean = op == "==" || op == "!=" || op == "<" || op == ">"
                         || op == "<=" || op == ">=" || op == "&&" || op == "||";

    // 整数どうしの場合 (演算の符号で，符号付きで演算するならint，符号なしならunsigned int)
    if (!is_lhs_pointer && !is_rhs_pointer) {
        expr->type = type_t{BASE_INT, is_boolean || is_signed_operation(op, lhs, rhs)};
        return;
    }
    // 論理演算の場合 (ポインタはnullptrを偽として真偽を判定する)
    if (op == "&&" || op == "||") {
        expr->type = type_t{BASE_INT, true};
        return;
    }
    // 一致の比較の場合 (同じ型のポインタどうしか，ポインタとnullptrに限る)
    if (op == "==" || op == "!=") {
        const bool has_nullptr = lhs.base == BASE_NULLPTR || rhs.base == BASE_NULLPTR;   // nullptrとの比較か
        if (!is_lhs_pointer || !is_rhs_pointer || (!has_nullptr && !Analyzer::is_same_type(lhs, rhs))) {
            throw std::string("compiler error: comparison between '") + type_to_string(lhs) + "' and '"
                  + type_to_string(rhs) + "' at " + loc_to_string(expr->loc);
        }
        expr->type = type_t{BASE_INT, true};
        return;
    }
    // 大小比較の場合 (同じ配列を指す同じ型のポインタどうしに限る．関数の番地とnullptrは大小に意味を持たない)
    if (is_boolean) {
        if (!is_pointer_value(lhs) || is_func_pointer(lhs) || !Analyzer::is_same_type(lhs, rhs)) {
            throw std::string("compiler error: comparison between '") + type_to_string(lhs) + "' and '"
                  + type_to_string(rhs) + "' at " + loc_to_string(expr->loc);
        }
        expr->type = type_t{BASE_INT, true};
        return;
    }
    // ポインタに整数を足し引きする場合 (ポインタ+整数・整数+ポインタ・ポインタ-整数)
    const bool is_pointer_offset =
        (op == "+" || op == "-") && is_pointer_value(lhs) && !is_func_pointer(lhs) && !is_rhs_pointer;   // ポインタ±整数か
    // (加算は左右を入れ替えても結果が同じ(n + pとp + nは同じ番地)ため，C言語と同じく整数+ポインタも許す．
    //  整数-ポインタは意味を持たないため許さない)
    const bool is_offset_pointer =
        op == "+" && !is_lhs_pointer && is_pointer_value(rhs) && !is_func_pointer(rhs);                  // 整数+ポインタか
    if (is_pointer_offset || is_offset_pointer) {
        expr->type = is_pointer_offset ? lhs : rhs;   // 結果は足し引きされるポインタの型
        return;
    }
    // ポインタどうしの差の場合 (同じ配列を指す同じ型のポインタどうしで，間の要素数をintで返す)
    if (op == "-" && is_pointer_value(lhs) && !is_func_pointer(lhs) && Analyzer::is_same_type(lhs, rhs)) {
        expr->type = type_t{BASE_INT, true};
        return;
    }
    // それ以外の演算にポインタを使った場合
    throw std::string("compiler error: invalid operands to binary '") + op + "' ('" + type_to_string(lhs)
          + "' and '" + type_to_string(rhs) + "') at " + loc_to_string(expr->loc);
}

// 構造体のメンバを名前から探す (見つからなければexprの位置でエラー)
const struct_member_t &Analyzer::find_member(const std::string &struct_name, const node_t *expr) const {
    for (const struct_member_t &member : this->struct_defs_.at(struct_name).members) {
        if (member.name == expr->sval) return member;
    }
    throw std::string("compiler error: struct '") + struct_name
          + "' has no member '" + expr->sval + "' at " + loc_to_string(expr->loc);
}

// 関数名を値として書いた式の型(その関数を指す関数ポインタの型)を返す
type_t Analyzer::func_pointer_type(const std::string &name) const {
    type_t type;
    type.base = BASE_FUNC;
    type.pointer_depth = 1;
    type.func_sig = this->func_sigs_.at(name);
    return type;
}

// 式srcの値を，型dstの格納先(代入・初期化する変数，関数の引数，戻り値)へ格納できるか検査する
// 整数どうしは型が異なっても格納でき(格納先の型の幅で格納する)，格納先と値のどちらかがポインタ(nullptrを含む)の
// 場合のみ，ポインタと整数の取り違えと指す先の型の違いを検査する
void Analyzer::check_assignable(const type_t &dst, const node_t *src, const std::string &context) {
    const type_t &value = src->type;   // 格納する値の型
    // 格納先がポインタの場合 (指す先の型が一致するポインタか，nullptrのみ格納できる)
    if (is_pointer_value(dst)) {
        // nullptrの場合 (どのポインタにも格納できる)
        if (value.base == BASE_NULLPTR) return;
        // 整数を格納しようとした場合 (ポインタと整数の間に暗黙の変換はなく，0もヌルポインタとしては扱わない)
        if (!is_pointer_value(value)) {
            throw std::string("compiler error: cannot convert '") + type_to_string(value) + "' to '"
                  + type_to_string(dst) + "' in " + context + " (use nullptr for a null pointer) at "
                  + loc_to_string(src->loc);
        }
        // 指す先の型が異なる場合
        if (!Analyzer::is_same_type(dst, value)) {
            throw std::string("compiler error: incompatible pointer types in ") + context + ": expected '"
                  + type_to_string(dst) + "' but got '" + type_to_string(value) + "' at " + loc_to_string(src->loc);
        }
        return;
    }
    // 格納先が整数で，ポインタを格納しようとした場合 (ポインタと整数の間に暗黙の変換はない)
    if (is_pointer_like(value)) {
        throw std::string("compiler error: cannot convert '") + type_to_string(value) + "' to '"
              + type_to_string(dst) + "' in " + context + " at " + loc_to_string(src->loc);
    }
}

// 2つの型が(ポインタの指す先を含め)同じかを返す
// 整数型は符号も含めて比べる(指す先の型で読み書きの幅と符号拡張が決まるため)
bool Analyzer::is_same_type(const type_t &a, const type_t &b) {
    // 基本型・ポインタの段数・配列かどうかのいずれかが異なる場合
    if (a.base != b.base || a.pointer_depth != b.pointer_depth || a.is_array != b.is_array) return false;
    // 構造体の場合
    if (a.base == BASE_STRUCT) return a.struct_name == b.struct_name;
    // 関数ポインタの場合 (戻り値型と引数型の並びが一致すること)
    if (a.base == BASE_FUNC) {
        const func_sig_t &sig_a = *a.func_sig;   // aのシグネチャ
        const func_sig_t &sig_b = *b.func_sig;   // bのシグネチャ
        // 戻り値型が異なる場合
        if (!Analyzer::is_same_type(sig_a.return_type, sig_b.return_type)) return false;
        // 引数の個数が異なる場合
        if (sig_a.param_types.size() != sig_b.param_types.size()) return false;
        // 引数の型を先頭から順に比べ，1つでも異なる場合
        for (size_t i = 0; i < sig_a.param_types.size(); i++) {
            if (!Analyzer::is_same_type(sig_a.param_types[i], sig_b.param_types[i])) return false;
        }
        return true;
    }
    // 整数型の場合 (符号の有無も一致すること)
    return a.is_signed == b.is_signed;
}

// 式を検査し，名前解決と型注釈を行う
// ノード種別ごとに固有の検査を行い，子を持つノードは子へ再帰する．
//   リテラル: 末端なので何もしない
//   変数参照・代入・インクリメント/デクリメント・関数呼び出し: それぞれ固有の検査を行う
//   二項演算・三項演算など(default): 固有の検査はなく，子を再帰検査するだけ
// 式文(a + b; のような文)もanalyze_stmtからこの関数で検査される
// 配列は配列の型のまま注釈する．値として使う文脈では，呼び出し側がanalyze_valueで先頭要素へのポインタとして扱う
void Analyzer::analyze_expr(node_t *expr) {
    switch (expr->kind) {
        // リテラル: 検査は不要だが，後段のコード生成のため型を注釈する
        case ND_INT_LIT:
            // 整数リテラルはintまたはunsigned int
            expr->type = type_t{BASE_INT, is_signed_literal(expr->ival)};
            return;
        case ND_CHAR_LIT:
            expr->type = type_t{BASE_CHAR, true};   // 文字リテラルはchar(符号付き)
            return;
        // nullptr: どのポインタ型とも比較・代入できる専用の型を持つ
        case ND_NULLPTR:
            expr->type = type_t{BASE_NULLPTR};
            return;
        // 関数の番地: 型は注釈済み (直接呼び出しの呼び出し先として置き換えた後に，再び検査される場合)
        case ND_FUNC_ADDR:
            return;

        // 文字列リテラル: 匿名のグローバルchar配列としてメモリを確保する
        case ND_STRING_LIT: {
            // サイズは文字列長 + 1(ヌル終端)
            type_t str_type = {BASE_CHAR, true, true, static_cast<int>(expr->sval.size()) + 1};
            const int bytes = Analyzer::calc_array_words(str_type) * 4;   // 文字列が占めるバイト数
            symbol_t *sym = new symbol_t{"", str_type, LOC_GLOBAL,
                                         this->alloc_var(bytes, LOC_GLOBAL), true, false};
            expr->sym = sym;
            expr->type = str_type;
            return;
        }

        // sizeof: 型のバイト数をコンパイル時に確定するintリテラルとして扱う (実行時命令は生成しない)
        case ND_SIZEOF: {
            int size;
            if (expr->children.empty()) {
                // sizeof(型名): パーサがexpr->typeに型を格納済み
                size = type_size_bytes(expr->type, this->struct_defs_);
            } else {
                // sizeof(変数名): 値ではなく型だけが必要なのでND_VARのみ許可する
                // (analyze_exprではなくlookup_symbolで直接型を取得する．readable=falseの
                //  書き込み専用ハードウェア変数(LED等)もsizeofの対象になり得るため)
                node_t *inner = expr->children[0];
                if (inner->kind != ND_VAR) {
                    throw std::string("compiler error: sizeof argument must be a type name or "
                                       "variable name at ") + loc_to_string(inner->loc);
                }
                const symbol_t *sym = this->lookup_symbol(inner->sval);
                if (sym == nullptr) {
                    throw std::string("compiler error: use of undeclared identifier '")
                          + inner->sval + "' at " + loc_to_string(inner->loc);
                }
                inner->sym  = sym;
                inner->type = sym->type;
                size = type_size_bytes(sym->type, this->struct_defs_);
            }
            expr->ival = size;
            expr->type = type_t{BASE_INT, true};   // sizeofの結果はint
            return;
        }

        // 変数参照: 名前を解決し，読み取り可能か確認する
        case ND_VAR: {
            const symbol_t *sym = this->lookup_symbol(expr->sval);
            if (sym == nullptr) {
                // 関数名の場合 (呼び出しの括弧を付けずに書いた関数名は，その関数の番地(関数ポインタ)として扱う)
                if (this->func_names_.count(expr->sval)) {
                    // mainの場合 (プログラムの開始点であり，呼び出し元へ復帰できないため呼び出す手段を与えない)
                    if (expr->sval == "main") {
                        throw std::string("compiler error: cannot take the address of 'main' at ")
                              + loc_to_string(expr->loc);
                    }
                    expr->kind = ND_FUNC_ADDR;
                    expr->type = this->func_pointer_type(expr->sval);
                    this->addr_taken_funcs_.insert(expr->sval);
                    return;
                }
                throw std::string("compiler error: use of undeclared identifier '")
                      + expr->sval + "' at " + loc_to_string(expr->loc);
            }
            if (!sym->readable) {
                throw std::string("compiler error: '") + expr->sval
                      + "' is not readable at " + loc_to_string(expr->loc);
            }
            // const変数はメモリを持たないため，参照そのものを値の整数リテラルに置き換える
            // (式の中での符号の扱いが変わらないよう，リテラルの型は宣言した型のままにする)
            if (sym->location == LOC_CONST) {
                expr->kind = ND_INT_LIT;
                expr->ival = Analyzer::const_symbol_value(sym);
                expr->type = sym->type;
                expr->type.is_const = false;
                return;
            }
            expr->sym  = sym;        // 名前解決の結果を結びつける
            expr->type = sym->type;  // 型を注釈する
            return;
        }

        // 番地の取得 &x: 対象が番地を持つ左辺値であることを確かめ，指す先をその型とするポインタの型を付ける
        case ND_ADDR_OF: {
            node_t *operand = expr->children[0];   // 番地を取る式
            // 関数名の場合 (関数の番地は&を付けずに関数名だけで書く．同じ値の書き方を1つにするため&は受け付けない)
            if (operand->kind == ND_VAR && this->func_names_.count(operand->sval)) {
                throw std::string("compiler error: cannot apply '&' to function '") + operand->sval
                      + "'; write the function name alone to get its address at " + loc_to_string(expr->loc);
            }
            this->analyze_lvalue(operand);
            // ハードウェア変数・const変数の場合 (メモリ上の番地を持たないため)
            if (operand->kind == ND_VAR && operand->sym->location != LOC_GLOBAL
                && operand->sym->location != LOC_LOCAL && operand->sym->location != LOC_PARAM) {
                throw std::string("compiler error: cannot take the address of '") + operand->sval
                      + "' (it has no memory address) at " + loc_to_string(expr->loc);
            }
            // 配列そのもの(&arr)の場合 (配列へのポインタは非対応．先頭要素の番地は配列の名前arrだけで書け，
            // 要素の番地は&arr[i]で取れる)
            if (operand->type.is_array) {
                throw std::string("compiler error: cannot take the address of an array; "
                                   "use the array itself or the address of its first element at ")
                      + loc_to_string(expr->loc);
            }
            // 関数ポインタの場合 (関数ポインタへのポインタは非対応)
            if (is_func_pointer(operand->type)) {
                throw std::string("compiler error: pointer to function pointer is not supported at ")
                      + loc_to_string(expr->loc);
            }
            expr->type = operand->type;
            expr->type.is_const = false;
            expr->type.pointer_depth++;
            return;
        }

        // 間接参照 *p: ポインタの指す先を表す (読み書きの対象は指す先の型になる)
        case ND_DEREF: {
            node_t *pointer = expr->children[0];   // 間接参照するポインタの式
            this->analyze_value(pointer);
            // 関数ポインタの場合 (指す先の関数は値として読み書きできず，呼び出しは関数ポインタのままfp(x)と書くため)
            if (is_func_pointer(pointer->type)) {
                throw std::string("compiler error: cannot dereference a function pointer; call it directly at ")
                      + loc_to_string(expr->loc);
            }
            // ポインタの値でない場合 (整数・nullptr等．nullptrはどのポインタにも入れられる値であり，
            // 指す先の型を持たないため間接参照できない)
            if (!is_pointer_value(pointer->type)) {
                throw std::string("compiler error: cannot dereference '") + type_to_string(pointer->type)
                      + "' at " + loc_to_string(expr->loc);
            }
            expr->type = pointee_type(pointer->type);
            return;
        }

        // 構造体メンバアクセス(a.b)の意味解析．メンバの型(expr->type)と，読み書き先を求めるための情報を確定させる
        // 基底(a)の形で次の4つの経路に分かれる
        //   (1) 単一の構造体変数 entry.member: 番地がコンパイル時に決まるため，メンバの番地を持つシンボル
        //       (名前・型・番地・読み書き可否をまとめた，名前解決の結果)を合成する
        //   (2) 構造体配列・構造体ポインタ変数の要素 arr[i].member・p[i].member
        //   (3) 構造体ポインタの指す先 p->member・(*p).member
        //   (4) 基底の式に添字を付けた要素 s.items[i].member・(*pp)[i].member
        //   (2)〜(4)は番地が実行時に決まるため，番地はコード生成が構造体定義から求めたメンバのオフセットを使って求める
        case ND_MEMBER_ACCESS: {
            node_t *base = expr->children[0];   // メンバの前についている構造体変数・構造体配列要素・間接参照

            // (3) 構造体ポインタの指す先のメンバ: p->member，(*p).member
            if (base->kind == ND_DEREF) {
                this->analyze_expr(base);
                // 指す先が構造体でない場合
                if (base->type.base != BASE_STRUCT || base->type.pointer_depth != 0) {
                    throw std::string("compiler error: member access requires a pointer to struct, but got '")
                          + type_to_string(base->children[0]->type) + "' at " + loc_to_string(expr->loc);
                }
                // メンバの型を注釈する (番地はコード生成側で，ポインタの値とメンバのオフセットから実行時計算する)
                expr->type = this->find_member(base->type.struct_name, expr).type;
                return;
            }

            // (4) 基底の式に添字を付けた要素のメンバ: s.items[i].member，(*pp)[i].member
            if (base->kind == ND_ARRAY_ACCESS && base->children.size() == 2) {
                this->analyze_expr(base);
                // 要素が構造体でない場合
                if (base->type.base != BASE_STRUCT || base->type.pointer_depth != 0) {
                    throw std::string("compiler error: member access requires a struct, but got '")
                          + type_to_string(base->type) + "' at " + loc_to_string(expr->loc);
                }
                expr->type = this->find_member(base->type.struct_name, expr).type;
                return;
            }

            const symbol_t *base_sym;           // 基底(構造体変数・構造体配列全体・構造体ポインタ)のシンボル
            if (base->kind == ND_ARRAY_ACCESS) {
                // (2) 構造体配列の要素へのメンバアクセス: arr[i].member (構造体ポインタの添字p[i].memberを含む)
                base_sym = this->lookup_symbol(base->sval);   // 配列全体を名前解決する
                if (base_sym == nullptr) {
                    throw std::string("compiler error: use of undeclared identifier '")
                          + base->sval + "' at " + loc_to_string(base->loc);
                }
                // 構造体の配列でも構造体へのポインタでもない変数へのarr[i].member形式のアクセスは禁止する
                const bool is_struct_array = base_sym->type.is_array && base_sym->type.base == BASE_STRUCT
                                          && base_sym->type.pointer_depth == 0;   // 構造体の配列か
                const bool is_struct_pointer = is_pointer_value(base_sym->type) && base_sym->type.base == BASE_STRUCT
                                            && base_sym->type.pointer_depth == 1;   // 構造体へのポインタか
                if (!is_struct_array && !is_struct_pointer) {
                    throw std::string("compiler error: '") + base->sval
                          + "' is not an array of struct at " + loc_to_string(base->loc);
                }
                base->sym  = base_sym;         // 配列全体のシンボルを結びつける
                base->type = base_sym->type;   // 型を注釈する
                // 添字(実行時に評価される)を検査する (void値・ポインタは添字に使えない)
                node_t *index_expr = base->children[0];
                this->analyze_value(index_expr);
                if (is_pointer_like(index_expr->type)) {
                    throw std::string("compiler error: array index must be an integer at ")
                          + loc_to_string(index_expr->loc);
                }
            } else {
                // (1) 単一の構造体変数へのメンバアクセス: entry.member
                base_sym = this->lookup_symbol(base->sval);   // 構造体変数自体を名前解決する
                if (base_sym == nullptr) {
                    throw std::string("compiler error: use of undeclared identifier '")
                          + base->sval + "' at " + loc_to_string(base->loc);
                }
                // 構造体へのポインタの場合 (指す先のメンバは->で参照する)
                if (base_sym->type.base == BASE_STRUCT && is_pointer_value(base_sym->type)) {
                    throw std::string("compiler error: '") + base->sval
                          + "' is a pointer to struct; use '->' to access its member at " + loc_to_string(base->loc);
                }
                // 構造体型でない変数へのメンバアクセス(例: intの変数にx.yと書く)は禁止する
                if (base_sym->type.base != BASE_STRUCT || base_sym->type.pointer_depth != 0) {
                    throw std::string("compiler error: '") + base->sval
                          + "' is not a struct at " + loc_to_string(base->loc);
                }
                base->sym  = base_sym;         // 名前解決の結果を結びつける
                base->type = base_sym->type;   // 型を注釈する
            }

            // 構造体定義からメンバ名が一致するものを探す (定義に存在しないメンバ名を指定した場合はエラー)
            const struct_member_t &member = this->find_member(base_sym->type.struct_name, expr);

            if (base->kind == ND_ARRAY_ACCESS) {
                // (2) 配列全体のシンボルとメンバの型を注釈する (番地はコード生成側で，メンバのオフセットから実行時計算する)
                expr->sym  = base_sym;
                expr->type = member.type;
            } else {
                // (1) 構造体変数の番地にメンバのオフセットを加えた番地を持つシンボルを合成する
                // (置き場所(location)・読み書き可否は構造体変数自身のものをそのまま引き継ぐ)
                symbol_t *sym = new symbol_t{
                    base_sym->name + "." + expr->sval,             // エラーメッセージ表示用の名前(例: "p.x")
                    member.type,                                   // メンバの型 (スカラー・ポインタまたは固定長配列)
                    base_sym->location,
                    base_sym->address + member.offset_words * 4,   // 番地 = 構造体変数の番地 + メンバのオフセット
                    base_sym->readable,
                    base_sym->writable,
                };
                expr->sym  = sym;
                expr->type = member.type;
            }
            return;
        }

        // 代入: 左辺は書き込み可能なスカラー(整数またはポインタ)の左辺値でなければならない
        case ND_ASSIGN: {
            node_t *lhs = expr->children[0];   // 代入先(左辺)
            // 左辺を名前解決する
            // (変数はanalyze_exprの変数参照を通さない．値を読み出す側の検査であり，const変数を値のリテラルに置き換え，
            //  読み取り不可の変数をエラーにする．書き込み先にその検査を当てると誤るが，
            //  読み出し側では必要な振る舞いのため，analyze_expr自体は変えられない)
            this->analyze_lvalue(lhs);
            // 配列・構造体そのものへの代入を禁止する
            Analyzer::check_scalar_operand(lhs, "assignment");
            // 書き込みできない左辺には代入できない (シンボルを持たないポインタの指す先は，常に書き込める)
            if (lhs->sym != nullptr && !lhs->sym->writable) {
                throw std::string("compiler error: '") + lhs->sym->name
                      + "' is not writable at " + loc_to_string(lhs->loc);
            }
            // 複合代入(+=等)は左辺を読みもするので，読み取り可能でもなければならない
            if (expr->sval != "=" && lhs->sym != nullptr && !lhs->sym->readable) {
                throw std::string("compiler error: '") + lhs->sym->name
                      + "' is not readable at " + loc_to_string(lhs->loc);
            }
            node_t *rhs = expr->children[1];  // 右辺
            this->analyze_value(rhs);         // 右辺を検査する (void関数の戻り値(値を持たない)を代入することはできない)
            // 単純代入の場合 (右辺を左辺の型の格納先へ格納できること)
            if (expr->sval == "=") {
                Analyzer::check_assignable(lhs->type, rhs, "assignment");
            }
            // ポインタへの複合代入の場合 (整数の+=/-=で指す先をずらすことのみ許す)
            else if (is_pointer_value(lhs->type)) {
                if ((expr->sval != "+=" && expr->sval != "-=") || is_func_pointer(lhs->type)
                    || is_pointer_like(rhs->type)) {
                    throw std::string("compiler error: invalid operands to '") + expr->sval + "' ('"
                          + type_to_string(lhs->type) + "' and '" + type_to_string(rhs->type) + "') at "
                          + loc_to_string(expr->loc);
                }
            }
            // 整数への複合代入に，ポインタを使った場合
            else if (is_pointer_like(rhs->type)) {
                throw std::string("compiler error: invalid operands to '") + expr->sval + "' ('"
                      + type_to_string(lhs->type) + "' and '" + type_to_string(rhs->type) + "') at "
                      + loc_to_string(expr->loc);
            }
            // 代入式の値の型は左辺の型とする
            expr->type = lhs->type;
            return;
        }

        // インクリメント・デクリメント: 対象は読み書き両方可能なスカラー(整数またはポインタ)の左辺値でなければならない
        case ND_UNOP:
        case ND_POST_UNOP:
            if (expr->sval == "++" || expr->sval == "--") {
                node_t *operand = expr->children[0];
                this->analyze_lvalue(operand);
                Analyzer::check_scalar_operand(operand, "'" + expr->sval + "'");
                // 関数ポインタの場合 (関数の番地をずらす意味がないため)
                if (is_func_pointer(operand->type)) {
                    throw std::string("compiler error: '") + expr->sval
                          + "' is not supported for function pointer at " + loc_to_string(expr->loc);
                }
                // 読み書きできない対象の場合 (シンボルを持たないポインタの指す先は，常に読み書きできる)
                if (operand->sym != nullptr && (!operand->sym->readable || !operand->sym->writable)) {
                    throw std::string("compiler error: '") + operand->sym->name
                          + "' is not readable and writable at " + loc_to_string(expr->loc);
                }
                expr->type = operand->type;
                return;
            }
            // その他の前置単項演算子(-, +, !, ~): 子を検査し，void値の使用を禁止する
            this->analyze_value(expr->children[0]);
            // ポインタの場合 (!はnullptrかどうかの判定として許し，-・+・~は値に意味がないためエラー)
            if (expr->sval != "!" && is_pointer_like(expr->children[0]->type)) {
                throw std::string("compiler error: invalid operand to unary '") + expr->sval + "' ('"
                      + type_to_string(expr->children[0]->type) + "') at " + loc_to_string(expr->loc);
            }
            // !の結果は0/1のint，それ以外はオペランドを整数昇格した型
            expr->type = type_t{BASE_INT, expr->sval == "!" || is_promoted_signed(expr->children[0]->type)};
            return;

        // 関数呼び出し: 直接呼び出しか関数ポインタ経由かを判定し，引数と戻り値を検査する
        case ND_CALL:
            this->analyze_call(expr);
            return;

        // 組み込み関数print: char配列(直接配列)のみ対応．ヌル終端まで出力する
        case ND_PRINT: {
            node_t *target = expr->children[0];
            this->check_char_array_operand(target, "print");
            // printは値を返さない(void)．戻り値を式として使うコードを既存のvoidチェック経路で検出させる
            expr->type = type_t{BASE_VOID, true};
            return;
        }

        // 組み込み関数streq: 2つのchar配列(直接配列)の内容が一致するか比較する
        case ND_STREQ: {
            node_t *lhs = expr->children[0];
            node_t *rhs = expr->children[1];
            this->check_char_array_operand(lhs, "streq");
            this->check_char_array_operand(rhs, "streq");
            // streqは0/1のint値を返す(if文の条件式等にそのまま使える)
            expr->type = type_t{BASE_INT, true};
            return;
        }

        // 組み込み関数strcopy: 第2引数(src)の内容を第1引数(dst)へヌル終端付きでコピーする
        case ND_STRCOPY: {
            node_t *dst = expr->children[0];
            node_t *src = expr->children[1];
            this->check_char_array_operand(dst, "strcopy");
            this->check_char_array_operand(src, "strcopy");
            if (!dst->sym->writable) {
                throw std::string("compiler error: '") + dst->sym->name
                      + "' is not writable at " + loc_to_string(dst->loc);
            }
            // strcopyは値を返さない(void)．戻り値を式として使うコードを既存のvoidチェック経路で検出させる
            expr->type = type_t{BASE_VOID, true};
            return;
        }

        // 組み込み関数scan: char配列(直接配列，2要素以上)のみ対応．改行までの1行をヌル終端付きで格納する
        // 書き込み可能性・最小サイズの検査が必要なためcheck_char_array_operandは使わず専用の検査を行う
        case ND_SCAN: {
            node_t *target = expr->children[0];   // 格納先配列 (parserがND_VARで構築)
            const symbol_t *sym = this->lookup_symbol(target->sval);
            if (sym == nullptr) {
                throw std::string("compiler error: use of undeclared identifier '")
                      + target->sval + "' at " + loc_to_string(target->loc);
            }
            if (!sym->writable) {
                throw std::string("compiler error: '") + target->sval
                      + "' is not writable at " + loc_to_string(target->loc);
            }
            target->sym  = sym;        // 名前解決の結果を結びつける
            target->type = sym->type;
            // ポインタの場合 (指す先の配列の大きさが分からず，打ち切りの判定ができないため)
            if (is_pointer_value(sym->type)) {
                throw std::string("compiler error: scan does not support pointers (the array size is unknown) at ")
                      + loc_to_string(target->loc);
            }
            // char型の配列でない場合 (charへのポインタの配列は，要素が文字ではないため対象外)
            if (!sym->type.is_array || sym->type.base != BASE_CHAR || sym->type.pointer_depth > 0) {
                throw std::string("compiler error: scan requires a char array at ")
                      + loc_to_string(target->loc);
            }
            if (sym->type.array_size < 2) {
                throw std::string("compiler error: scan target array must have at least 2 elements "
                                   "(1 for content plus 1 for null terminator) at ")
                      + loc_to_string(target->loc);
            }
            // scanは値を返さない(void)．戻り値を式として使うコードを既存のvoidチェック経路で検出させる
            expr->type = type_t{BASE_VOID, true};
            return;
        }

        // 配列要素アクセス: 添字を付ける対象(配列またはポインタ)の名前解決とインデックス式の検査を行う
        // 配列変数・ポインタ変数はsval(変数名)で解決し，それ以外(メンバ・間接参照・ポインタの配列の要素)は
        // 基底の式children[1]を検査して解決する
        case ND_ARRAY_ACCESS: {
            type_t base_type;   // 添字を付ける対象(配列またはポインタ)の型
            if (expr->children.size() == 2) {
                // 基底の式: children[1]を検査させる．
                // 基底が構造体のメンバの場合，symは通常のメンバなら「メンバ自身」，構造体配列要素のメンバなら
                // 「配列全体」を指す(コード生成でのアドレス計算用)ため，配列判定・要素型には
                // 基底の型(常にメンバ自身の型)を使う
                node_t *base = expr->children[1];
                this->analyze_expr(base);
                expr->sym = base->sym;
                base_type = base->type;
            } else {
                // 配列変数・ポインタ変数: svalに入っている変数名で名前解決する
                const symbol_t *sym = this->lookup_symbol(expr->sval);
                if (sym == nullptr) {
                    throw std::string("compiler error: use of undeclared identifier '")
                          + expr->sval + "' at " + loc_to_string(expr->loc);
                }
                expr->sym = sym;
                base_type = sym->type;
            }
            // 要素の型を求める
            // (構造体の配列の要素・構造体へのポインタに添字を付けた要素(p[i])は構造体そのものであり，
            //  メンバアクセスか&を介してのみ使える．値として使った場合はanalyze_valueがエラーにする)
            if (base_type.is_array) {
                expr->type = base_type;
                expr->type.is_array = false;
                expr->type.array_size = 0;
            } else if (is_func_pointer(base_type)) {
                // 関数ポインタに添字を付けた場合 (関数の番地は，要素を並べたメモリ上の領域を指さないため)
                throw std::string("compiler error: function pointer cannot be indexed at ") + loc_to_string(expr->loc);
            } else if (is_pointer_value(base_type)) {
                expr->type = pointee_type(base_type);
            } else {
                const std::string name = expr->sval.empty() ? "expression" : "'" + expr->sval + "'";   // エラーメッセージに書く対象
                throw std::string("compiler error: ") + name
                      + " is not an array or pointer at " + loc_to_string(expr->loc);
            }
            // 添字の式を検査する (void値(戻り値のない関数呼び出し)・ポインタは添字に使えない)
            node_t *index_expr = expr->children[0];   // 添字の式
            this->analyze_value(index_expr);
            if (is_pointer_like(index_expr->type)) {
                throw std::string("compiler error: array index must be an integer at ")
                      + loc_to_string(index_expr->loc);
            }
            return;
        }

        // 二項演算: 両辺を値として検査し，整数・ポインタの組み合わせに応じて結果の型を決める
        case ND_BINOP:
            this->analyze_binop(expr);
            return;

        // 三項演算 a ? b : c: 条件・両分岐を値として検査し，両分岐の型から結果の型を決める
        case ND_TERNARY: {
            this->analyze_value(expr->children[0]);
            this->analyze_value(expr->children[1]);
            this->analyze_value(expr->children[2]);
            const type_t &then_type = expr->children[1]->type;   // then節の型
            const type_t &else_type = expr->children[2]->type;   // else節の型
            // 整数どうしの場合 (結果は両方の分岐の値が昇格後intならint，そうでなければunsigned int)
            if (!is_pointer_like(then_type) && !is_pointer_like(else_type)) {
                expr->type = type_t{BASE_INT, is_promoted_signed(then_type) && is_promoted_signed(else_type)};
                return;
            }
            // ポインタを含む場合 (同じ型のポインタどうし，またはポインタとnullptrに限り，結果はそのポインタの型)
            const bool is_then_null = then_type.base == BASE_NULLPTR;   // then節がnullptrか
            const bool is_else_null = else_type.base == BASE_NULLPTR;   // else節がnullptrか
            const bool is_compatible = is_pointer_like(then_type) && is_pointer_like(else_type)
                && (is_then_null || is_else_null || Analyzer::is_same_type(then_type, else_type));   // 両分岐の型が揃うか
            if (!is_compatible) {
                throw std::string("compiler error: type mismatch in conditional expression ('")
                      + type_to_string(then_type) + "' and '" + type_to_string(else_type) + "') at "
                      + loc_to_string(expr->loc);
            }
            expr->type = is_then_null ? else_type : then_type;
            return;
        }

        // 到達しない (式ノードの全種類は上記いずれかのcaseで処理される)．
        // 将来式ノードを追加した際に検査漏れとなるのを防ぐため，未対応として即エラーにする
        default:
            throw std::string("compiler error: unsupported expression node kind at ")
                  + loc_to_string(expr->loc);
    }
}

// 名前からシンボルを探す (内側のローカルスコープから順に，最後にグローバル・ハードウェア変数)
const symbol_t *Analyzer::lookup_symbol(const std::string &name) const {
    // ローカルスコープを内側から外側へ探す
    for (auto it = this->scopes_.rbegin(); it != this->scopes_.rend(); ++it) {
        const auto found = it->find(name);
        if (found != it->end()) return found->second;
    }
    // グローバル変数・ハードウェア変数を探す
    const auto found = this->symbols_.find(name);
    if (found != this->symbols_.end()) return found->second;
    return nullptr;
}
