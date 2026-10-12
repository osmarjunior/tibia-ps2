#!/usr/bin/env bash
set -euo pipefail

echo "=== OTHire Server (Tibia 7.4) ==="

DB_HOST="${DB_HOST:-db}"
DB_PORT="${DB_PORT:-3306}"
DB_USER="${DB_USER:-othire}"
DB_NAME="${DB_NAME:-othire}"
DB_PASSWORD="${DB_PASSWORD:?DB_PASSWORD não definida}"
DB_ROOT_PASSWORD="${DB_ROOT_PASSWORD:-}"      # só necessário p/ carregar o schema aqui
SERVER_IP="${SERVER_IP:-127.0.0.1}"            # IP anunciado aos clientes (IP da máquina na LAN)
PASSWORD_TYPE="${PASSWORD_TYPE:-plain}"        # plain | md5 | sha1 (como o cliente envia/DB guarda)
SEED_TEST_DATA="${SEED_TEST_DATA:-0}"
WAIT_TIMEOUT="${WAIT_TIMEOUT:-120}"

case "$PASSWORD_TYPE" in
    plain|md5|sha1) ;;
    *) echo "ERRO: PASSWORD_TYPE inválido: $PASSWORD_TYPE (use plain, md5 ou sha1)"; exit 1 ;;
esac
[[ "$DB_PORT" =~ ^[0-9]+$ ]] || { echo "ERRO: DB_PORT inválida: $DB_PORT"; exit 1; }

# Escapa caracteres especiais do sed (\ & |) nos valores
esc() { printf '%s' "$1" | sed -e 's/[\\&|]/\\&/g'; }

# ---------------------------------------------------------------------
# 1. Gera config.lua a partir do template
#    - sql_*: conexão com o banco
#    - ip: endereço anunciado aos clientes na lista de personagens
#    - bind_only_global_address=false: escuta em todas as interfaces do
#      container (com true, ficaria preso a um IP que não existe aqui
#      dentro e o Docker não conseguiria encaminhar as conexões)
# ---------------------------------------------------------------------
cp /otserv/config.lua.default /otserv/config.lua
sed -i \
    -e "s|^\([[:space:]]*sql_type[[:space:]]*=\).*|\1 \"mysql\"|" \
    -e "s|^\([[:space:]]*sql_host[[:space:]]*=\).*|\1 \"$(esc "$DB_HOST")\"|" \
    -e "s|^\([[:space:]]*sql_port[[:space:]]*=\).*|\1 ${DB_PORT}|" \
    -e "s|^\([[:space:]]*sql_user[[:space:]]*=\).*|\1 \"$(esc "$DB_USER")\"|" \
    -e "s|^\([[:space:]]*sql_pass[[:space:]]*=\).*|\1 \"$(esc "$DB_PASSWORD")\"|" \
    -e "s|^\([[:space:]]*sql_db[[:space:]]*=\).*|\1 \"$(esc "$DB_NAME")\"|" \
    -e "s|^\([[:space:]]*ip[[:space:]]*=\).*|\1 \"$(esc "$SERVER_IP")\"|" \
    -e "s|^\([[:space:]]*bind_only_global_address[[:space:]]*=\).*|\1 false|" \
    -e "s|^\([[:space:]]*passwordtype[[:space:]]*=\).*|\1 \"${PASSWORD_TYPE}\"|" \
    /otserv/config.lua

for key in sql_host sql_port sql_user sql_pass sql_db ip bind_only_global_address passwordtype; do
    grep -q "^[[:space:]]*${key}[[:space:]]*=" /otserv/config.lua \
        || { echo "ERRO: chave '${key}' não encontrada no config.lua"; exit 1; }
done

echo "IP anunciado aos clientes: ${SERVER_IP}"
echo "Tipo de senha das contas: ${PASSWORD_TYPE}"
[ "$PASSWORD_TYPE" = "plain" ] && echo "AVISO: senhas em texto puro no banco (ok só para testes)."
if [ "$SERVER_IP" = "127.0.0.1" ]; then
    echo "AVISO: SERVER_IP=127.0.0.1 só funciona para clientes na própria máquina."
    echo "       Para o PS2/outros PCs, defina SERVER_IP com o IP da máquina na rede."
fi

# ---------------------------------------------------------------------
# 2. Espera o banco (com timeout). Senha via MYSQL_PWD, não aparece no ps.
# ---------------------------------------------------------------------
export MYSQL_PWD="$DB_PASSWORD"
MYSQL=(mysql -h "$DB_HOST" -P "$DB_PORT" -u "$DB_USER" "$DB_NAME")

echo "Aguardando banco em ${DB_HOST}:${DB_PORT} (timeout ${WAIT_TIMEOUT}s)..."
elapsed=0
until "${MYSQL[@]}" -e 'SELECT 1' >/dev/null 2>&1; do
    if [ "$elapsed" -ge "$WAIT_TIMEOUT" ]; then
        echo "ERRO: banco não respondeu em ${WAIT_TIMEOUT}s. Último erro:"
        "${MYSQL[@]}" -e 'SELECT 1' || true
        exit 1
    fi
    sleep 2
    elapsed=$((elapsed + 2))
done
echo "Banco pronto."

# ---------------------------------------------------------------------
# 3. Schema (preferível: montar o .sql em /docker-entrypoint-initdb.d
#    do container MariaDB, como no docker-compose; aqui fica como fallback)
# ---------------------------------------------------------------------
if ! "${MYSQL[@]}" -e 'SELECT 1 FROM accounts LIMIT 1' >/dev/null 2>&1; then
    echo "Schema ausente."
    if [ -z "$DB_ROOT_PASSWORD" ]; then
        echo "ERRO: defina DB_ROOT_PASSWORD para carregar o schema, ou use initdb.d no MariaDB."
        exit 1
    fi
    echo "Carregando schema..."
    MYSQL_PWD="$DB_ROOT_PASSWORD" mysql -h "$DB_HOST" -P "$DB_PORT" -u root "$DB_NAME" \
        < /otserv/MySql_Schema.sql
    echo "Schema carregado."
fi

# ---------------------------------------------------------------------
# 4. Dados de teste (opcional: SEED_TEST_DATA=1). Idempotente.
#    Conta de login: 111111 / senha: tibia, gravada conforme PASSWORD_TYPE
#    (plain = texto puro; precisa bater com passwordtype do config.lua).
#    A trigger oncreate_players cria as skills do personagem.
# ---------------------------------------------------------------------
if [ "$SEED_TEST_DATA" = "1" ]; then
    case "$PASSWORD_TYPE" in
        plain) PW_SQL="'tibia'" ;;
        md5)   PW_SQL="MD5('tibia')" ;;
        sha1)  PW_SQL="SHA1('tibia')" ;;
    esac
    echo "Inserindo conta/personagem de teste (111111 / tibia, ${PASSWORD_TYPE})..."
    "${MYSQL[@]}" <<SQL
INSERT INTO accounts (id, password, email, premend, blocked, warnings)
VALUES (111111, ${PW_SQL}, '', 0, 0, 0)
ON DUPLICATE KEY UPDATE password = VALUES(password);

INSERT IGNORE INTO players
  (name, account_id, group_id, sex, vocation, experience, level, maglevel,
   health, healthmax, mana, manamax, cap,
   lookhead, lookbody, looklegs, lookfeet, looktype,
   posx, posy, posz, conditions, town_id, rank_id, guildnick)
VALUES
  ('Ps2 Tester', 111111, 1, 1, 0, 4200, 8, 0,
   185, 185, 35, 35, 470,
   10, 20, 30, 40, 129,
    996, 1000, 7, '', 1, 0, '');
SQL
fi

# Não vaza a senha para o processo do servidor
unset MYSQL_PWD DB_PASSWORD DB_ROOT_PASSWORD

echo "Iniciando OTHire..."
cd /otserv
exec ./otserv