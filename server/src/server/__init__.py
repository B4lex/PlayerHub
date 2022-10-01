def main() -> None:
    import uvicorn

    uvicorn.run("server.app:app", host="0.0.0.0", port=3000)
