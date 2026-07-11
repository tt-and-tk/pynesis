// 意味解析エラー: scanは値を返さない(void)ため，二項演算のオペランドに使えない (異常系)
char buf[8];
void main(void) {
    int x;
    x = 1 + scan(buf);
}
