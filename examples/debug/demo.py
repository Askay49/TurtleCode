"""Set a breakpoint on line 7, then inspect values and step into add()."""
def add(left, right):
    result = left + right
    return result

values = [3, 5, 8]
total = 0
for value in values:
    total = add(total, value)
    print(f"value={value}, total={total}")
print(f"Done: {total}")
