// 関数の前方参照 (ソース上で後ろに定義された関数を先に呼び出す，引数検査が正しく機能することを確認)
void main(void) {
    int x = add(3, 5);
}
int add(int a, int b) {
    return a + b;
}
