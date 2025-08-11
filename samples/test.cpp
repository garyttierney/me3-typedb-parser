class SomeParentClass {
  int v;
  virtual void help() = 0;
  virtual int test() { return v * 2; }
};

class SomePureVirtualClass {
public:
  virtual ~SomePureVirtualClass() = default;
  virtual void start() = 0;
  virtual void do2() = 0;
};

class MyClass : SomePureVirtualClass, SomeParentClass {

public:
  void help() override;
  void start() override;

  void do2() override;
};

enum MyEnum {
  v1,
  v2,
  v3,
  v4,
};

void MyClass::help() {}
void MyClass::start() {}
void MyClass::do2() {}
