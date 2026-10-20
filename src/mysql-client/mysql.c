

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mysql.h>

// 连接参数从 config.h 读取。该文件已被 .gitignore 忽略，不会进入版本库。
// 首次使用请把 config.h.example 复制为 config.h 并填入自己的配置。
#include "config.h"


#define SQL_INSERT_TBL_USER		"INSERT TBL_USER(U_NAME, U_GENDER) VALUES('King', 'man');"
#define SQL_SELECT_TBL_USER		"SELECT * FROM TBL_USER;"

#define SQL_DELETE_TBL_USER		"CALL PROC_DELETE_USER('King')"
#define SQL_INSERT_IMG_USER		"INSERT TBL_USER(U_NAME, U_GENDER, U_IMG) VALUES('King', 'man', ?);"

#define SQL_SELECT_IMG_USER		"SELECT U_IMG FROM TBL_USER WHERE U_NAME='King';"


#define FILE_IMAGE_LENGTH		(64*1024)
// C U R D --> 
// 

int king_mysql_select(MYSQL *handle) { //

	// mysql_real_query --> sql
	if (mysql_real_query(handle, SQL_SELECT_TBL_USER, strlen(SQL_SELECT_TBL_USER))) {
		printf("mysql_real_query : %s\n", mysql_error(handle));
		return -1;
	}
	

	// store --> 
	MYSQL_RES *res = mysql_store_result(handle);
	if (res == NULL) {
		printf("mysql_store_result : %s\n", mysql_error(handle));
		return -2;
	}

	// rows / fields
	int rows = mysql_num_rows(res);
	printf("rows: %d\n", rows);
	
	int fields = mysql_num_fields(res);
	printf("fields: %d\n", fields);

	// fetch
	MYSQL_ROW row;
	while ((row = mysql_fetch_row(res))) {

		int i = 0;
		for (i = 0;i < fields;i ++) {
			printf("%s\t", row[i]);
		}
		printf("\n");
		
	}

	mysql_free_result(res);

	return 0;
}


// filename : path + file name
// buffer   : store image data
// capacity : size of buffer, used for bounds checking

int read_image(const char *filename, char *buffer, int capacity) {

	if (filename == NULL || buffer == NULL || capacity <= 0) return -1;
	
	FILE *fp = fopen(filename, "rb"); //
	if (fp == NULL) {
		printf("fopen failed: %s\n", filename);
		return -2;
	}

	// file size
	fseek(fp, 0, SEEK_END);
	long length = ftell(fp); // file size
	fseek(fp, 0, SEEK_SET);

	if (length < 0) {
		printf("ftell failed\n");
		fclose(fp);
		return -3;
	}

	// 边界检查：文件长度不能超过调用方给出的缓冲区容量
	if (length > (long)capacity) {
		printf("read_image: %s is %ld bytes, but buffer capacity is %d\n",
			filename, length, capacity);
		fclose(fp);
		return -4;
	}

	int size = (int)fread(buffer, 1, (size_t)length, fp);
	fclose(fp);

	if (size != (int)length) {
		printf("fread failed: %d\n", size);
		return -5;
	}

	return size;

}

// filename :
// buffer : 
// length :

int write_image(const char *filename, char *buffer, int length) {

	if (filename == NULL || buffer == NULL || length <= 0) return -1;

	FILE *fp = fopen(filename, "wb+"); //
	if (fp == NULL) {
		printf("fopen failed: %s\n", filename);
		return -2;
	}

	int size = fwrite(buffer, 1, length, fp);
	fclose(fp);

	if (size != length) {
		printf("fwrite failed: %d\n", size);
		return -3;
	}

	return size;
}

int mysql_write(MYSQL *handle, char *buffer, int length) {

	if (handle == NULL || buffer == NULL || length <= 0) return -1;

	MYSQL_STMT *stmt = mysql_stmt_init(handle);
	if (stmt == NULL) {
		printf("mysql_stmt_init : %s\n", mysql_error(handle));
		return -2;
	}

	int ret = mysql_stmt_prepare(stmt, SQL_INSERT_IMG_USER, strlen(SQL_INSERT_IMG_USER));
	if (ret) {
		printf("mysql_stmt_prepare : %s\n", mysql_stmt_error(stmt));
		mysql_stmt_close(stmt);
		return -3;
	}

	MYSQL_BIND param = {0};
	param.buffer_type  = MYSQL_TYPE_LONG_BLOB;
	param.buffer = NULL;
	param.is_null = 0;
	param.length = NULL;

	ret = mysql_stmt_bind_param(stmt, &param);
	if (ret) {
		printf("mysql_stmt_bind_param : %s\n", mysql_stmt_error(stmt));
		mysql_stmt_close(stmt);
		return -4;
	}

	ret = mysql_stmt_send_long_data(stmt, 0, buffer, length);
	if (ret) {
		printf("mysql_stmt_send_long_data : %s\n", mysql_stmt_error(stmt));
		mysql_stmt_close(stmt);
		return -5;
	}

	ret = mysql_stmt_execute(stmt);
	if (ret) {
		printf("mysql_stmt_execute : %s\n", mysql_stmt_error(stmt));
		mysql_stmt_close(stmt);
		return -6;
	}

	if (mysql_stmt_close(stmt)) {
		printf("mysql_stmt_close : %s\n", mysql_error(handle));
		return -7;
	}
	
	return 0;
}

int mysql_read(MYSQL *handle, char *buffer, int capacity) {

	if (handle == NULL || buffer == NULL || capacity <= 0) return -1;

	MYSQL_STMT *stmt = mysql_stmt_init(handle);
	if (stmt == NULL) {
		printf("mysql_stmt_init : %s\n", mysql_error(handle));
		return -2;
	}

	int ret = mysql_stmt_prepare(stmt, SQL_SELECT_IMG_USER, strlen(SQL_SELECT_IMG_USER));
	if (ret) {
		printf("mysql_stmt_prepare : %s\n", mysql_stmt_error(stmt));
		mysql_stmt_close(stmt);
		return -3;
	}

	
	MYSQL_BIND result = {0};
	
	result.buffer_type  = MYSQL_TYPE_LONG_BLOB;
	unsigned long total_length = 0;
	result.length = &total_length;

	ret = mysql_stmt_bind_result(stmt, &result);
	if (ret) {
		printf("mysql_stmt_bind_result : %s\n", mysql_stmt_error(stmt));
		mysql_stmt_close(stmt);
		return -4;
	}

	ret = mysql_stmt_execute(stmt);
	if (ret) {
		printf("mysql_stmt_execute : %s\n", mysql_stmt_error(stmt));
		mysql_stmt_close(stmt);
		return -5;
	}

	ret = mysql_stmt_store_result(stmt);
	if (ret) {
		printf("mysql_stmt_store_result : %s\n", mysql_stmt_error(stmt));
		mysql_stmt_close(stmt);
		return -6;
	}


	int read_length = 0;

	while (1) {

		ret = mysql_stmt_fetch(stmt);
		if (ret != 0 && ret != MYSQL_DATA_TRUNCATED) break; // 

		// 本次取到的字段实际长度，fetch 之后才有确定值
		unsigned long wanted = total_length;

		// 边界检查：数据库里的内容不能超过调用方给出的缓冲区容量
		if ((long)wanted > (long)capacity) {
			printf("mysql_read: column is %lu bytes, but buffer capacity is %d\n",
				wanted, capacity);
			mysql_stmt_close(stmt);
			return -7;
		}

		// 按区间把数据搬进调用方的缓冲区。
		// 原实现把 buffer_length 固定为 1，每调用一次只取回一个字节，
		// 读满 64KB 需要 65536 次调用；这里一次请求整段剩余长度。
		unsigned long offset = 0;
		while (offset < wanted) {

			unsigned long remaining = wanted - offset;

			result.buffer = buffer + offset;
			result.buffer_length = remaining;

			if (mysql_stmt_fetch_column(stmt, &result, 0, offset)) {
				printf("mysql_stmt_fetch_column : %s\n", mysql_stmt_error(stmt));
				mysql_stmt_close(stmt);
				return -8;
			}

			offset += remaining;
		}

		read_length = (int)wanted;
	}

	mysql_stmt_close(stmt);

	return read_length;

}

int main() {

	MYSQL mysql;
	char *buffer = NULL;
	int length = 0;

	if (NULL == mysql_init(&mysql)) {
		printf("mysql_init : %s\n", mysql_error(&mysql));
		return -1;
	}

	if (!mysql_real_connect(&mysql, KING_DB_SERVER_IP, KING_DB_USERNAME, KING_DB_PASSWORD, 
		KING_DB_DEFAULTDB, KING_DB_SERVER_PORT, NULL, 0)) {

		printf("mysql_real_connect : %s\n", mysql_error(&mysql));
		goto Exit;
	}

	// mysql --> insert 
	printf("case : mysql --> insert \n");
#if 1
	if (mysql_real_query(&mysql, SQL_INSERT_TBL_USER, strlen(SQL_INSERT_TBL_USER))) {
		printf("mysql_real_query : %s\n", mysql_error(&mysql));
		goto Exit;
	}
#endif

	king_mysql_select(&mysql);

	// mysql --> delete 

	printf("case : mysql --> delete \n");
#if 1
	if (mysql_real_query(&mysql, SQL_DELETE_TBL_USER, strlen(SQL_DELETE_TBL_USER))) {
		printf("mysql_real_query : %s\n", mysql_error(&mysql));
		goto Exit;
	}
#endif
	
	king_mysql_select(&mysql);



	printf("case : mysql --> read image and write mysql\n");
	
	// 图片缓冲区从堆上分配。栈空间有限（Windows 默认 1MB、Linux 8MB），
	// 在上面放 64KB 的数组不合适
	buffer = (char *)malloc(FILE_IMAGE_LENGTH);
	if (buffer == NULL) {
		printf("malloc : out of memory\n");
		goto Exit;
	}
	
	length = read_image("0voice.jpg", buffer, FILE_IMAGE_LENGTH);
	if (length < 0) goto Exit;
	
	mysql_write(&mysql, buffer, length); /// 


	printf("case : mysql --> read mysql and write image\n");
	
	memset(buffer, 0, FILE_IMAGE_LENGTH);
	length = mysql_read(&mysql, buffer, FILE_IMAGE_LENGTH);

	write_image("a.jpg", buffer, length);

Exit:
	free(buffer);
	mysql_close(&mysql);

	return 0;

}


