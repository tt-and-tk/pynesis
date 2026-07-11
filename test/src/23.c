// signed修飾子の明示指定 (戻り値型・パラメータ型・変数宣言の3箇所全てで指定，デフォルトのsignedと同じ意味になる)
signed int sadd(signed int a, signed int b) {
    signed int sum = a + b;
    return sum;
}
void main(void) {
    signed int x = sadd(3, 5);
}
