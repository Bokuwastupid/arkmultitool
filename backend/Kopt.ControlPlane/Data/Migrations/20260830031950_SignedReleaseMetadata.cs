using Microsoft.EntityFrameworkCore.Migrations;

#nullable disable

namespace Kopt.ControlPlane.Data.Migrations
{
    /// <inheritdoc />
    public partial class SignedReleaseMetadata : Migration
    {
        /// <inheritdoc />
        protected override void Up(MigrationBuilder migrationBuilder)
        {
            migrationBuilder.AddColumn<string>(
                name: "Changelog",
                table: "Releases",
                type: "text",
                nullable: true);

            migrationBuilder.AddColumn<string>(
                name: "KnownIssues",
                table: "Releases",
                type: "text",
                nullable: true);

            migrationBuilder.AddColumn<string>(
                name: "MinimumLoaderVersion",
                table: "Releases",
                type: "text",
                nullable: false,
                defaultValue: "");
        }

        /// <inheritdoc />
        protected override void Down(MigrationBuilder migrationBuilder)
        {
            migrationBuilder.DropColumn(
                name: "Changelog",
                table: "Releases");

            migrationBuilder.DropColumn(
                name: "KnownIssues",
                table: "Releases");

            migrationBuilder.DropColumn(
                name: "MinimumLoaderVersion",
                table: "Releases");
        }
    }
}
