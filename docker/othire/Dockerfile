# Dockerfile for building and running OTHire (Tibia 7.4 server)

# =====================================================================
# Stage 1: build
# =====================================================================
FROM ubuntu:22.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ make git ca-certificates \
    liblua5.1-0-dev libxml2-dev libgmp-dev \
    libsqlite3-dev libmysqlclient-dev \
    libboost-system-dev libboost-thread-dev libboost-regex-dev \
    && rm -rf /var/lib/apt/lists/*

# Para reprodutibilidade, fixe um commit: --build-arg OTHIRE_REF=<hash>
ARG OTHIRE_REF=master
RUN git clone --recursive https://github.com/Ezzz-dev/OTHire.git /othire \
    && git -C /othire checkout "${OTHIRE_REF}"

# Patch: aceitar protocolo 740 (cliente PS2 envia 740; default do OTHire é 772).
# O grep falha o build se o sed não casar (ex.: upstream mudou).
RUN sed -i 's/CLIENT_VERSION_MIN 772/CLIENT_VERSION_MIN 740/' /othire/source/protocol.h \
    && grep -q 'CLIENT_VERSION_MIN 740' /othire/source/protocol.h

# Patch: compatibilidade com boost 1.74 (enum anônimo -> int em seconds()).
RUN sed -i \
        -e 's/seconds(Connection::read_timeout)/seconds((int)Connection::read_timeout)/g' \
        -e 's/seconds(Connection::write_timeout)/seconds((int)Connection::write_timeout)/g' \
        /othire/source/connection.cpp \
    && grep -q '(int)Connection::read_timeout' /othire/source/connection.cpp \
    && grep -q '(int)Connection::write_timeout' /othire/source/connection.cpp

# Patch: o cliente MySQL 8.0 (Ubuntu 22.04) removeu o tipo my_bool; a opção
# MYSQL_OPT_RECONNECT agora recebe bool*.
RUN sed -i 's/my_bool reconnect/bool reconnect/' /othire/source/databasemysql.cpp \
    && grep -q 'bool reconnect = true' /othire/source/databasemysql.cpp

WORKDIR /othire/source

# Compila todos os .cpp direto (dispensa premake4).
# -w -fpermissive: código C++ de ~2014 não compila limpo em compilador novo.
# BOOST_BIND_GLOBAL_PLACEHOLDERS: silencia erro de deprecação do boost bind.
# __USE_MYSQL__: OBRIGATÓRIO. Sem ele o definitions.h cai no driver padrão
# (SQLite) e o servidor ignora o sql_type = "mysql" do config.lua.
RUN g++ -o otserv *.cpp \
    -I/usr/include/lua5.1 \
    -I/usr/include/libxml2 \
    -llua5.1 -lxml2 -lgmp -lsqlite3 -lmysqlclient \
    -lboost_system -lboost_thread -lboost_regex -lpthread \
    -std=gnu++0x -O2 -w -fpermissive \
    -DBOOST_BIND_GLOBAL_PLACEHOLDERS \
    -D__USE_MYSQL__ \
    -D__SKULLSYSTEM__ \
    -D__ENABLE_SERVER_DIAGNOSTIC__ \
    && nm -D otserv | grep -q mysql_real_connect

# =====================================================================
# Stage 2: runtime
# =====================================================================
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    liblua5.1-0 libxml2 libgmp10 libsqlite3-0 libmysqlclient21 \
    libboost-system1.74.0 libboost-thread1.74.0 libboost-regex1.74.0 \
    default-mysql-client \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --shell /usr/sbin/nologin otserv \
    && mkdir -p /otserv \
    && chown otserv:otserv /otserv

# O diretório precisa existir e pertencer ao otserv ANTES do WORKDIR:
# se o WORKDIR criar a pasta, ela fica como root e o servidor
# não consegue gravar o config.lua gerado nem logs.
WORKDIR /otserv

# Binário e dados do jogo (mapa, items.otb, scripts, XMLs...)
COPY --from=build --chown=otserv:otserv /othire/source/otserv /otserv/otserv
COPY --from=build --chown=otserv:otserv /othire/data /otserv/data

# config.lua é o template; o entrypoint gera o config.lua final a partir
# dele usando variáveis de ambiente (host/usuário/senha do banco).
COPY --chown=otserv:otserv config.lua /otserv/config.lua.default
COPY --chown=otserv:otserv MySql_Schema.sql /otserv/MySql_Schema.sql
COPY --chown=otserv:otserv --chmod=755 entrypoint.sh /otserv/entrypoint.sh

# Defaults (sobrescreva no docker-compose / .env; não use estes em produção)
ENV DB_HOST=db \
    DB_PORT=3306 \
    DB_USER=othire \
    DB_NAME=othire \
    DB_PASSWORD=changeme \
    SEED_TEST_DATA=0

USER otserv

EXPOSE 7171 7172

ENTRYPOINT ["/otserv/entrypoint.sh"]