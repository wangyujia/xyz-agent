def fib(n):
    """计算斐波那契数列第 n 项"""
    if n <= 0:
        return 0
    elif n == 1:
        return 1
    
    a, b = 0, 1
    for _ in range(2, n + 1):
        a, b = b, a + b
    return b

if __name__ == "__main__":
    # 测试 fib(10)
    result = fib(10)
    print(f"fib(10) = {result}")
    
    # 打印前几项验证
    print("斐波那契数列前10项:")
    for i in range(10):
        print(f"fib({i}) = {fib(i)}")