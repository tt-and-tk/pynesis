// 関数呼び出し (複数関数，グローバル・ローカル変数)
int g = 0;
void helper(void) {
    int t = 5;      // ローカル変数
    g += t;         // グローバルを更新
}
void main(void) {
    int i;
    for (i = 0; i < 3; i += 1) {
        helper();   // 関数呼び出し
    }
}
