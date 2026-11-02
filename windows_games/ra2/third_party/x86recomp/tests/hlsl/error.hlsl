// Must fail to translate: an undeclared identifier.
float4 main(float4 c : COLOR0) : COLOR0 {
  return c * undeclared;
}
