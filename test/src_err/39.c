// 意味解析エラー: void関数の戻り値で変数を初期化することはできない (異常系)
void f(void) {
}
void main(void) {
    int x = f();
}
