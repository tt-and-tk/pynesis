// 構文解析エラー: []の前が変数名でない (異常系)
int a;
int b;
void main(void) {
    int x;
    x = (a + b)[0];
}
