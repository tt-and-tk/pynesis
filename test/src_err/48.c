// 意味解析エラー: void関数の戻り値を変数へ代入できない (異常系)
void f(void) {
}
void main(void) {
    int x;
    x = f();
}
