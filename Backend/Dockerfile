# Use the official ultra-fast uv image
FROM ghcr.io/astral-sh/uv:python3.10-bookworm-slim

RUN apt-get update && apt-get install -y libgl1 libglib2.0-0 libsm6 libxrender1 libxext6 && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Place the virtual environment outside of /app so it doesn't get overwritten 
# when docker-compose mounts your local src folder into /app.
ENV UV_PROJECT_ENVIRONMENT="/opt/venv"
ENV PATH="/opt/venv/bin:$PATH"

# Copy the uv configuration files
COPY pyproject.toml uv.lock ./

# Install dependencies using uv sync.
# --frozen ensures it strictly follows uv.lock for exactly reproducible builds
# --no-install-project skips treating the directory itself as a python package
RUN uv sync --frozen --no-install-project

EXPOSE 8000

# The actual source code (main.py) is mounted into /app at runtime via docker-compose
CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8000"]
