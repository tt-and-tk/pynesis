// 意味解析エラー: printは値を返さない(void)ため，戻り値を変数の初期化に使えない (異常系)
char buf[8];
void main(void) {
    int x = print(buf);
}
