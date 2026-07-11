// 標準入出力 (組み込み関数print/scan，グローバル・ローカルchar配列)
char g[8];          // グローバル配列 (scanの格納先)
void main(void) {
    char x[8];      // ローカル配列 (scanの格納先)
    scan(g);        // 標準入力(1行) → グローバル配列g
    scan(x);        // 標準入力(1行) → ローカル配列x
    print(g);       // グローバル配列を出力
    print(x);       // ローカル配列を出力
}
