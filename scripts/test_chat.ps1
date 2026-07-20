$body = @{
    user_id = 'u1'
    message = '今晚三个人吃海鲜，预算300左右，上海'
    city = '上海'
} | ConvertTo-Json -Compress

Invoke-RestMethod -Uri http://localhost:8080/v1/chat -Method Post -ContentType 'application/json' -Body $body
