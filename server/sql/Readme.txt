1.: my.cnf sql_mode & bind-address:

It is highly recommended to modify sql_mode to sql_mode=NO_ENGINE_SUBSTITUTION in your
/usr/local/etc/mysql/my.cnf (MariaDB 10.6) or /usr/local/my.cnf (MySQL5.6) or you will have DATA TRUNCATED errors.
On MariaDB, you'll need the "bind-address = *" line for enabling connection to MySQL from outside the server's local network.
If you don't have the my.cnf file, just create it, here's the content what you should put:
MariaDB 10.6: https://pastebin.com/ps9ZyX4U
MySQL 5.6: https://pastebin.com/xNsP6pVA
(These example files also provided in this zip.)

2.: Create a user for the serverfile:

You have to create a metin2@localhost MySQL user with every Server Priviliges in order to make the serverfile work.
For password use the word "password" or you can change the password in CONFIG files and use your own password.

a.) You can create this user in navicat -> user -> new user
Fill the username (metin2), the host (localhost), and the password (password) then on Server Priviliges tab tick everything and click Save.

b.) Or you can create it in the FreeBSD console:
mysql -u root -p
(It will ask for your local root password, if it's empty then just press enter)
CREATE USER 'metin2'@'localhost' IDENTIFIED BY 'password';
GRANT ALL PRIVILEGES ON *.* TO 'metin2'@'localhost' WITH GRANT OPTION;
FLUSH PRIVILEGES;

If you want a remote user (like root@% or somebody@%), you can create it the same way:
mysql -u root -p
CREATE USER 'root'@'%' IDENTIFIED BY '123456789';
GRANT ALL PRIVILEGES ON *.* TO 'root'@'%' WITH GRANT OPTION;
FLUSH PRIVILEGES;

3.: How to import the databases:

a.) You can import them in the FreeBSD console: (preferred way)
mysql -u root -p
(It will ask for your local root password, if it's empty then just press enter)
CREATE DATABASE account;
CREATE DATABASE common;
CREATE DATABASE player;
CREATE DATABASE log;
CREATE DATABASE hotbackup;
exit
(Yes, don't forget hotbackup, it's needed even if empty..)

Then import upload the .sqls to the server and import them:
mysql -u root -p account < /path/to/sql/account.sql
mysql -u root -p common < /path/to/sql/common.sql
mysql -u root -p player < /path/to/sql/player.sql
mysql -u root -p log < /path/to/sql/log.sql
(It will ask for your local root password, if it's empty then just press enter)

b.) Or use navicat: right click to the connection -> new database, once you created then right click to it -> execute sql file and select the .sql file.
