import json
import urllib.request

req = urllib.request.Request(
    "http://localhost:8080/v1/chat",
    data=json.dumps({
        "user_id": "u1",
        "message": "今晚三个人吃海鲜，预算300左右，上海",
        "city": "上海"
    }, ensure_ascii=False).encode("utf-8"),
    headers={"Content-Type": "application/json"},
    method="POST"
)

with urllib.request.urlopen(req) as resp:
    print(resp.read().decode("utf-8"))
