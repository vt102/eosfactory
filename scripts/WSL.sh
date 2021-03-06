printf "%s\n" "
         source scripts/${OS_NAME}.sh
"

function setLinuxVariable() {
    name=$1
    value=$2
    config=$(grep -Po '"$name":.*?[^\\]",' $EOSIO_EOSFACTORY_DIR__/$config_json)
    if [ -z "$config"  -a ! -z "$value" -a "${!name}" != "$value" ]; then
        export $name=$value
        echo export $name=$value >> ~/.profile
        printf "\t%s\n" "setting $name: $value"
    fi
}

